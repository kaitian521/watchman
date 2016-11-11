/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_synchronized.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>

#ifdef _WIN32

#define NETWORK_BUF_SIZE (64*1024)

struct WinWatcher : public Watcher {
  HANDLE ping{INVALID_HANDLE_VALUE}, olapEvent{INVALID_HANDLE_VALUE};
  HANDLE dir_handle{INVALID_HANDLE_VALUE};

  std::condition_variable cond;
  watchman::Synchronized<std::deque<w_string>, std::mutex> changedItems;

  explicit WinWatcher(w_root_t* root);
  ~WinWatcher();

  struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(w_root_t* root, PendingCollection::LockedPtr& coll)
      override;

  bool waitNotify(int timeoutms) override;
  bool start(w_root_t* root) override;
  void signalThreads() override;
  void readChangesThread(w_root_t* root);
};

WinWatcher::WinWatcher(w_root_t* root)
    : Watcher("win32", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
  WCHAR* wpath;
  int err;

  wpath = w_utf8_to_win_unc(root->root_path.data(), root->root_path.size());
  if (!wpath) {
    throw std::runtime_error(
        std::string("failed to convert root path to WCHAR: ") +
        win32_strerror(GetLastError()));
  }

  // Create an overlapped handle so that we can avoid blocking forever
  // in ReadDirectoryChangesW
  dir_handle = CreateFileW(
      wpath,
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (!dir_handle) {
    throw std::runtime_error(
        std::string("failed to open dir ") + root->root_path.c_str() + ": " +
        win32_strerror(GetLastError()));
  }

  ping = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!ping) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
  olapEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!olapEvent) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
}

WinWatcher::~WinWatcher() {
  if (ping != INVALID_HANDLE_VALUE) {
    CloseHandle(ping);
  }
  if (olapEvent != INVALID_HANDLE_VALUE) {
    CloseHandle(olapEvent);
  }
  if (dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(dir_handle);
  }
}

void WinWatcher::signalThreads() {
  SetEvent(ping);
}

void WinWatcher::readChangesThread(w_root_t* root) {
  DWORD size = WATCHMAN_BATCH_LIMIT * (sizeof(FILE_NOTIFY_INFORMATION) + 512);
  std::vector<uint8_t> buf;
  DWORD err, filter;
  OVERLAPPED olap;
  BOOL initiate_read = true;
  HANDLE handles[2] = {olapEvent, ping};
  DWORD bytes;

  w_set_thread_name("readchange %s", root->root_path.c_str());
  watchman::log(watchman::DBG, "initializing\n");

  // Block until winmatch_root_st is waiting for our initialization
  {
    auto wlock = changedItems.wlock();

    filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE;

    memset(&olap, 0, sizeof(olap));
    olap.hEvent = olapEvent;

    buf.resize(size);

    if (!ReadDirectoryChangesW(
            dir_handle, &buf[0], size, TRUE, filter, nullptr, &olap, nullptr)) {
      err = GetLastError();
      w_log(
          W_LOG_ERR,
          "ReadDirectoryChangesW: failed, cancel watch. %s\n",
          win32_strerror(err));
      root->cancel();
      return;
    }
    // Signal that we are done with init.  We MUST do this AFTER our first
    // successful ReadDirectoryChangesW, otherwise there is a race condition
    // where we'll miss observing the cookie for a query that comes in
    // after we've crawled but before the watch is established.
    w_log(W_LOG_DBG, "ReadDirectoryChangesW signalling as init done\n");
    cond.notify_one();
  }
  initiate_read = false;

  // The mutex must not be held when we enter the loop
  while (!root->inner.cancelled) {
    if (initiate_read) {
      if (!ReadDirectoryChangesW(
              dir_handle,
              &buf[0],
              size,
              TRUE,
              filter,
              nullptr,
              &olap,
              nullptr)) {
        err = GetLastError();
        w_log(W_LOG_ERR,
            "ReadDirectoryChangesW: failed, cancel watch. %s\n",
            win32_strerror(err));
        root->cancel();
        break;
      } else {
        initiate_read = false;
      }
    }

    watchman::log(watchman::DBG, "waiting for change notifications\n");
    DWORD status = WaitForMultipleObjects(2, handles, FALSE, 10000);
    watchman::log(watchman::DBG, "wait returned with status ", status, "\n");

    if (status == WAIT_OBJECT_0) {
      bytes = 0;
      if (!GetOverlappedResult(dir_handle, &olap, &bytes, FALSE)) {
        err = GetLastError();
        w_log(
            W_LOG_ERR,
            "overlapped ReadDirectoryChangesW(%s): 0x%x %s\n",
            root->root_path.c_str(),
            err,
            win32_strerror(err));

        if (err == ERROR_INVALID_PARAMETER && size > NETWORK_BUF_SIZE) {
          // May be a network buffer related size issue; the docs say that
          // we can hit this when watching a UNC path. Let's downsize and
          // retry the read just one time
          w_log(
              W_LOG_ERR,
              "retrying watch for possible network location %s "
              "with smaller buffer\n",
              root->root_path.c_str());
          size = NETWORK_BUF_SIZE;
          initiate_read = true;
          continue;
        }

        if (err == ERROR_NOTIFY_ENUM_DIR) {
          root->scheduleRecrawl("ERROR_NOTIFY_ENUM_DIR");
        } else {
          w_log(
              W_LOG_ERR, "Cancelling watch for %s\n", root->root_path.c_str());
          root->cancel();
          break;
        }
      } else {
        PFILE_NOTIFY_INFORMATION not = (PFILE_NOTIFY_INFORMATION)buf.data();
        std::deque<w_string> items;

        while (true) {
          DWORD n_chars;
          w_string_t* name;

          // FileNameLength is in BYTES, but FileName is WCHAR
          n_chars = not->FileNameLength / sizeof(not->FileName[0]);
          name = w_string_new_wchar_typed(not->FileName, n_chars, W_STRING_BYTE);

          auto full = w_string::pathCat({root->root_path, name});
          w_string_delref(name);

          if (!root->ignore.isIgnored(full.data(), full.size())) {
            items.emplace_back(std::move(full));
          }

          // Advance to next item
          if (not->NextEntryOffset == 0) {
            break;
          }
          not = (PFILE_NOTIFY_INFORMATION)(not->NextEntryOffset + (char*)not);
        }

        if (!items.empty()) {
          auto wlock = changedItems.wlock();
          std::move(items.begin(), items.end(), std::back_inserter(*wlock));
          cond.notify_one();
        }
        ResetEvent(olapEvent);
        initiate_read = true;
      }
    } else if (status == WAIT_OBJECT_0 + 1) {
      w_log(W_LOG_ERR, "signalled\n");
      break;
    } else if (status != WAIT_TIMEOUT) {
      w_log(W_LOG_ERR, "impossible wait status=%d\n", status);
      break;
    }
  }

  w_log(W_LOG_DBG, "done\n");
}

bool WinWatcher::start(w_root_t* root) {
  int err;
  unused_parameter(root);

  // Spin up the changes reading thread; it owns a ref on the root
  w_root_addref(root);

  try {
    // Acquire the mutex so thread initialization waits until we release it
    auto wlock = changedItems.wlock();

    watchman::log(watchman::DBG, "starting readChangesThread\n");
    auto self = std::dynamic_pointer_cast<WinWatcher>(shared_from_this());
    std::thread thread([self, root]() {
      try {
        self->readChangesThread(root);
      } catch (const std::exception& e) {
        watchman::log(watchman::ERR, "uncaught exception: ", e.what());
        root->cancel();
      }

      // Ensure that we signal the condition variable before we
      // finish this thread.  That ensures that don't get stuck
      // waiting in WinWatcher::start if something unexpected happens.
      auto wlock = self->changedItems.wlock();
      self->cond.notify_one();

      w_root_delref_raw(root);
    });
    // We have to detach because the readChangesThread may wind up
    // being the last thread to reference the watcher state and
    // cannot join itself.
    thread.detach();

    // Allow thread init to proceed; wait for its signal
    if (cond.wait_for(wlock.getUniqueLock(), std::chrono::seconds(10)) ==
        std::cv_status::timeout) {
      watchman::log(
          watchman::ERR, "timedout waiting for readChangesThread to start\n");
      root->cancel();
      return false;
    }

    if (root->failure_reason) {
      w_log(
          W_LOG_ERR,
          "failed to start readchanges thread: %s\n",
          root->failure_reason.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    w_root_delref_raw(root);
    w_log(W_LOG_ERR, "failed to start readchanges thread: %s\n", e.what());
    return false;
  }
}

struct watchman_dir_handle* WinWatcher::startWatchDir(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, strerror(errno));
    return nullptr;
  }

  return osdir;
}

bool WinWatcher::consumeNotify(
    w_root_t* root,
    PendingCollection::LockedPtr& coll) {
  std::deque<w_string> items;
  struct timeval now;

  {
    auto wlock = changedItems.wlock();
    std::swap(items, *wlock);
  }

  gettimeofday(&now, nullptr);

  for (auto& item : items) {
    watchman::log(watchman::DBG, "readchanges: add pending ", item, "\n");
    coll->add(item, now, W_PENDING_VIA_NOTIFY);
  }

  return !items.empty();
}

bool WinWatcher::waitNotify(int timeoutms) {
  auto wlock = changedItems.wlock();
  cond.wait_for(wlock.getUniqueLock(), std::chrono::milliseconds(timeoutms));
  return !wlock->empty();
}

static RegisterWatcher<WinWatcher> reg("win32");

#endif // _WIN32

/* vim:ts=2:sw=2:et:
 */
