// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "git.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#include "check.h"
#include "scope_guard.h"
#include "thread_pool.h"
#include "timer.h"

namespace gitstatus {

namespace {

using namespace std::string_literals;

static ThreadPool g_thread_pool;

template <class T>
T Load(const std::atomic<T>& x) {
  return x.load(std::memory_order_relaxed);
}

template <class T>
void Store(std::atomic<T>& x, T v) {
  x.store(v, std::memory_order_relaxed);
}

template <class T>
void Inc(std::atomic<T>& x) {
  x.fetch_add(1, std::memory_order_relaxed);
}

template <class T>
void Dec(std::atomic<T>& x) {
  x.fetch_sub(1, std::memory_order_relaxed);
}

git_index* Index(git_repository* repo) {
  git_index* res = nullptr;
  VERIFY(!git_repository_index(&res, repo)) << GitError();
  return res;
}

}  // namespace

const char* GitError() {
  // There is no git_error_last() on OSX, so we use a deprecated alternative.
  const git_error* err = giterr_last();
  return err && err->message ? err->message : "unknown error";
}

const char* RepoState(git_repository* repo) {
  // These names mostly match gitaction in vcs_info:
  // https://github.com/zsh-users/zsh/blob/master/Functions/VCS_Info/Backends/VCS_INFO_get_data_git.
  switch (git_repository_state(repo)) {
    case GIT_REPOSITORY_STATE_NONE:
      return "";
    case GIT_REPOSITORY_STATE_MERGE:
      return "merge";
    case GIT_REPOSITORY_STATE_REVERT:
      return "revert";
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
      return "revert-seq";
    case GIT_REPOSITORY_STATE_CHERRYPICK:
      return "cherry";
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
      return "cherry-seq";
    case GIT_REPOSITORY_STATE_BISECT:
      return "bisect";
    case GIT_REPOSITORY_STATE_REBASE:
      return "rebase";
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
      return "rebase-i";
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
      return "rebase-m";
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
      return "am";
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE:
      return "am/rebase";
  }
  return "action";
}

size_t CountRange(git_repository* repo, const std::string& range) {
  git_revwalk* walk = nullptr;
  VERIFY(!git_revwalk_new(&walk, repo)) << GitError();
  ON_SCOPE_EXIT(=) { git_revwalk_free(walk); };
  VERIFY(!git_revwalk_push_range(walk, range.c_str())) << GitError();
  size_t res = 0;
  while (true) {
    git_oid oid;
    switch (git_revwalk_next(&oid, walk)) {
      case 0:
        ++res;
        break;
      case GIT_ITEROVER:
        return res;
      default:
        LOG(ERROR) << "git_revwalk_next: " << range << ": " << GitError();
        throw Exception();
    }
  }
}

git_repository* OpenRepo(const std::string& dir) {
  git_repository* repo = nullptr;
  switch (git_repository_open_ext(&repo, dir.c_str(), GIT_REPOSITORY_OPEN_FROM_ENV, nullptr)) {
    case 0:
      return repo;
    case GIT_ENOTFOUND:
      return nullptr;
    default:
      LOG(ERROR) << "git_repository_open_ext: " << dir << ": " << GitError();
      throw Exception();
  }
}

size_t NumStashes(git_repository* repo) {
  size_t res = 0;
  auto* cb = +[](size_t index, const char* message, const git_oid* stash_id, void* payload) {
    ++*static_cast<size_t*>(payload);
    return 0;
  };
  VERIFY(!git_stash_foreach(repo, cb, &res)) << GitError();
  return res;
}

const char* RemoteUrl(git_repository* repo) {
  git_remote* remote = nullptr;
  switch (git_remote_lookup(&remote, repo, "origin")) {
    case 0:
      return git_remote_url(remote) ?: "";
    case GIT_ENOTFOUND:
    case GIT_EINVALIDSPEC:
      return "";
    default:
      LOG(ERROR) << "git_remote_lookup: " << GitError();
      throw Exception();
  }
}

git_reference* Head(git_repository* repo) {
  git_reference* head = nullptr;
  switch (git_repository_head(&head, repo)) {
    case 0:
      return head;
    case GIT_ENOTFOUND:
    case GIT_EUNBORNBRANCH:
      return nullptr;
    default:
      LOG(ERROR) << "git_repository_head: " << GitError();
      throw Exception();
  }
}

git_reference* Upstream(git_reference* local) {
  git_reference* upstream = nullptr;
  switch (git_branch_upstream(&upstream, local)) {
    case 0:
      return upstream;
    case GIT_ENOTFOUND:
      return nullptr;
    default:
      // There is no git_error_last() or GIT_ERROR_INVALID on OSX, so we use deprecated
      // alternatives.
      VERIFY(giterr_last()->klass == GITERR_INVALID) << "git_branch_upstream: " << GitError();
      return nullptr;
  }
}

const char* BranchName(const git_reference* ref) {
  const char* name = git_reference_name(ref);
  const char* sep = std::strrchr(name, '/');
  return sep ? sep + 1 : name;
}

bool HasStaged(git_repository* repo, git_reference* head, git_index* index) {
  const git_oid* oid = git_reference_target(head);
  VERIFY(oid);
  git_commit* commit = nullptr;
  VERIFY(!git_commit_lookup(&commit, repo, oid)) << GitError();
  ON_SCOPE_EXIT(=) { git_commit_free(commit); };
  git_tree* tree = nullptr;
  VERIFY(!git_commit_tree(&tree, commit)) << GitError();
  git_diff_options opt = GIT_DIFF_OPTIONS_INIT;
  opt.notify_cb = +[](const git_diff* diff, const git_diff_delta* delta,
                      const char* matched_pathspec, void* payload) -> int { return GIT_EUSER; };
  git_diff* diff = nullptr;
  switch (git_diff_tree_to_index(&diff, repo, tree, index, &opt)) {
    case 0:
      git_diff_free(diff);
      return false;
    case GIT_EUSER:
      return true;
    default:
      LOG(ERROR) << "git_diff_tree_to_index: " << GitError();
      throw Exception();
  }
}

Repo::Repo(git_repository* repo) try : repo_(repo), index_(Index(repo_)) {
  UpdateSplits();
} catch (...) {
  git_repository_free(repo);
  throw;
}

Repo::~Repo() {
  Wait(true);
  git_index_free(index_);
  git_repository_free(repo_);
}

void Repo::UpdateDirty() {
  Wait(true);

  constexpr unsigned kUntracked = GIT_STATUS_WT_NEW;
  constexpr unsigned kUnstaged = GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
                                 GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_RENAMED |
                                 GIT_STATUS_CONFLICTED;

  auto Update = [&](std::string& f1, std::string& f2, unsigned m1, unsigned m2) {
    if (f1.empty()) return false;
    unsigned int flags = 0;
    if (git_status_file(&flags, repo_, f1.c_str())) {
      f1.clear();
      return false;
    }
    if (flags & m2) {
      f2 = std::move(f1);
      f1.clear();
      return true;
    }
    if (!(flags & m1)) f1.clear();
    return false;
  };

  Update(unstaged_, untracked_, kUnstaged, kUntracked) ||
      Update(untracked_, unstaged_, kUntracked, kUnstaged);
  Store(has_unstaged_, !unstaged_.empty());
  Store(has_untracked_, !untracked_.empty());
}

void Repo::ScanDirty() {
  ON_SCOPE_EXIT(&) {
    if (HasUnstaged()) LOG(INFO) << "Unstaged: " << unstaged_;
    if (HasUntracked()) LOG(INFO) << "Untracked: " << untracked_;
  };

  Wait(true);

  if (HasUnstaged() && HasUntracked()) return;

  CHECK(Load(inflight_) == 0);
  Inc(inflight_);
  ON_SCOPE_EXIT(&) { DecInflight(); };

  git_diff_options opt = GIT_DIFF_OPTIONS_INIT;
  opt.payload = this;
  opt.flags = GIT_DIFF_SKIP_BINARY_CHECK;
  if (!HasUntracked()) {
    // We could remove GIT_DIFF_RECURSE_UNTRACKED_DIRS and manually check in OnDirty whether
    // the allegedly untracked file is an empty directory. Unfortunately, it'll break UpdateDirty()
    // because we cannot use git_status_file on a directory. Seems like there is no way to quickly
    // get any untracked file from a directory that definitely has untracked files.
    // git_diff_index_to_workdir actually computes this before telling us that a directory is
    // untracked, but it doesn't give us the file path.
    opt.flags |= GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
  }
  opt.ignore_submodules = GIT_SUBMODULE_IGNORE_DIRTY;
  opt.notify_cb = +[](const git_diff* diff, const git_diff_delta* delta,
                      const char* matched_pathspec, void* payload) -> int {
    return static_cast<Repo*>(payload)->OnDelta(delta->status, delta->new_file.path);
  };

  for (size_t i = 0; i != splits_.size() - 1; ++i) {
    RunAsync([this, opt, start = splits_[i], end = splits_[i + 1]]() mutable {
      opt.entry_start = start.c_str();
      opt.entry_end = end.c_str();
      git_diff* diff = nullptr;
      switch (git_diff_index_to_workdir(&diff, repo_, index_, &opt)) {
        case 0:
          git_diff_free(diff);
          break;
        case GIT_EUSER:
          break;
        default:
          LOG(ERROR) << "git_diff_index_to_workdir: " << GitError();
          throw Exception();
      }
    });
  }

  Wait(false);
  RunAsync([this] { UpdateSplits(); });
}

bool Repo::HasUnstaged() const { return Load(has_unstaged_); }
bool Repo::HasUntracked() const { return Load(has_untracked_); }

void Repo::UpdateSplits() {
  constexpr size_t kEntriesPerShard = 1024;

  size_t n = git_index_entrycount(index_);
  if (n <= kEntriesPerShard || g_thread_pool.num_threads() < 2) {
    splits_ = {""s, ""s};
    return;
  }

  std::vector<char*> entries(n);
  std::vector<char*> patches;
  patches.reserve(8 * n);
  for (size_t i = 0; i != n; ++i) {
    char* path = const_cast<char*>(git_index_get_byindex(index_, i)->path);
    if (std::strchr(path, 1)) {
      splits_ = {""s, ""s};
      return;
    }
    entries[i] = path;
    while ((path = std::strchr(path, '/'))) {
      static_assert(std::is_unsigned<char>());
      patches.push_back(path);
      *path = 1;
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const char* x, const char* y) { return std::strcmp(x, y) < 0; });
  for (char* p : patches) *p = '/';

  size_t shards = std::min(n / kEntriesPerShard + 1, 2 * g_thread_pool.num_threads());
  splits_.clear();
  splits_.reserve(shards + 1);
  splits_.push_back("");
  for (size_t i = 0; i != shards - 1; ++i) {
    splits_.push_back(entries[(i + 1) * n / shards]);
  }
  splits_.push_back("");
  CHECK(splits_.size() == shards + 1);
}

void Repo::DecInflight() {
  std::unique_lock<std::mutex> lock(mutex_);
  CHECK(inflight_ > 0);
  if (--inflight_ < 2) cv_.notify_one();
}

void Repo::RunAsync(std::function<void()> f) {
  Inc(inflight_);
  try {
    g_thread_pool.Schedule([this, f = std::move(f)] {
      ON_SCOPE_EXIT(&) { DecInflight(); };
      f();
    });
  } catch (...) {
    DecInflight();
    throw;
  }
}

int Repo::OnDelta(git_delta_t status, const char* path) {
  VERIFY(path && *path);
  if (status == GIT_DELTA_UNTRACKED) {
    if (!HasUntracked()) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (untracked_.empty()) {
        untracked_ = path;
        Store(has_untracked_, true);
        if (HasUnstaged()) cv_.notify_one();
      }
    }
  } else {
    if (!HasUnstaged()) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (unstaged_.empty()) {
        unstaged_ = path;
        Store(has_unstaged_, true);
        if (HasUntracked()) cv_.notify_one();
      }
    }
  }
  return HasUnstaged() && HasUntracked() ? GIT_EUSER : 1;
}

void Repo::Wait(bool full_stop) {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    if (inflight_ == 0) break;
    if (!full_stop) {
      if (inflight_ == 1) break;
      if (HasUnstaged() && HasUntracked()) break;
    }
    cv_.wait(lock);
  }
  CHECK(HasUnstaged() == !unstaged_.empty());
  CHECK(HasUntracked() == !untracked_.empty());
}

}  // namespace gitstatus
