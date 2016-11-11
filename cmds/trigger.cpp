/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

bool watchman_trigger_command::waitNoIntr() {
  while (!w_is_stopping() && !stopTrigger_) {
    int status;
    auto pid = waitpid(current_proc, &status, 0);
    if (pid == current_proc) {
      current_proc = 0;
      return true;
    }
    if (errno != EINTR) {
      watchman::log(
          watchman::ERR,
          "waitpid returned ",
          errno,
          strerror(errno),
          " while waiting for child process pid ",
          current_proc,
          " trigger ",
          triggername,
          "\n");
      return false;
    }
  }
  return false;
}

void watchman_trigger_command::run(const w_root_t* root) {
  unlocked_watchman_root unlocked{const_cast<w_root_t*>(root)};

  w_set_thread_name(
      "trigger %s %s", triggername.c_str(), root->root_path.c_str());

  watchman_event_poll pfd[1];
  pfd[0].evt = ping_;

  watchman::log(watchman::DBG, "waiting for settle\n");

  while (!w_is_stopping() && !stopTrigger_) {
    ignore_result(w_poll_events(pfd, 1, 86400));
    if (w_is_stopping() || stopTrigger_) {
      break;
    }
    while (w_event_test_and_clear(ping_)) {
      while (auto item = subscriber_->getNext()) {
        if (!item->payload.get_default("settled")) {
          continue;
        }
        if (!maybeSpawn(&unlocked)) {
          continue;
        }
        waitNoIntr();
      }
    }
  }

  watchman::log(watchman::DBG, "out of loop\n");
}

/* trigger-del /root triggername
 * Delete a trigger from a root
 */
static void cmd_trigger_delete(
    struct watchman_client* client,
    const json_ref& args) {
  w_string tname;
  bool res;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments");
    w_root_delref(&unlocked);
    return;
  }
  auto jname = args.at(2);
  if (!json_is_string(jname)) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    w_root_delref(&unlocked);
    return;
  }
  tname = json_to_w_string(jname);

  {
    auto map = unlocked.root->triggers.wlock();
    auto it = map->find(tname);
    if (it == map->end()) {
      res = false;
    } else {
      // Stop the thread and erase it
      it->second->stop();
      map->erase(it);
      res = true;
    }
  }

  if (res) {
    w_state_save();
  }

  auto resp = make_response();
  resp.set({{"deleted", json_boolean(res)}, {"trigger", json_ref(jname)}});
  send_and_dispose_response(client, std::move(resp));
  w_root_delref(&unlocked);
}
W_CMD_REG("trigger-del", cmd_trigger_delete, CMD_DAEMON, w_cmd_realpath_root)

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
static void cmd_trigger_list(
    struct watchman_client* client,
    const json_ref& args) {
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  auto resp = make_response();
  w_root_read_lock(&unlocked, "trigger-list", &lock);
  auto arr = lock.root->triggerListToJson();
  w_root_read_unlock(&lock, &unlocked);

  resp.set("triggers", std::move(arr));
  send_and_dispose_response(client, std::move(resp));
  w_root_delref(&unlocked);
}
W_CMD_REG("trigger-list", cmd_trigger_list, CMD_DAEMON, w_cmd_realpath_root)

static json_ref build_legacy_trigger(
    w_root_t* root,
    struct watchman_client* client,
    const json_ref& args) {
  char *errmsg;
  uint32_t next_arg = 0;
  uint32_t i;
  size_t n;

  auto trig = json_object({{"name", args.at(2)},
                           {"append_files", json_true()},
                           {"stdin",
                            json_array({typed_string_to_json("name"),
                                        typed_string_to_json("exists"),
                                        typed_string_to_json("new"),
                                        typed_string_to_json("size"),
                                        typed_string_to_json("mode")})}});

  json_ref expr;
  auto query =
      w_query_parse_legacy(root, args, &errmsg, 3, &next_arg, nullptr, &expr);
  if (!query) {
    send_error_response(client, "invalid rule spec: %s", errmsg);
    free(errmsg);
    return nullptr;
  }

  json_object_set(trig, "expression", expr.get_default("expression"));

  if (next_arg >= args.array().size()) {
    send_error_response(client, "no command was specified");
    return nullptr;
  }

  n = json_array_size(args) - next_arg;
  auto command = json_array_of_size(n);
  for (i = 0; i < n; i++) {
    auto ele = args.at(i + next_arg);
    if (!json_is_string(ele)) {
      send_error_response(client, "expected argument %d to be a string", i);
      return nullptr;
    }
    json_array_append(command, ele);
  }
  json_object_set_new(trig, "command", std::move(command));

  return trig;
}

static bool parse_redirection(const char **name_p, int *flags,
  const char *label, char **errmsg)
{
  const char *name = *name_p;

  if (!name) {
    return true;
  }

  if (name[0] != '>') {
    ignore_result(asprintf(errmsg,
      "%s: must be prefixed with either > or >>, got %s",
      label, name));
    return false;
  }

  *flags = O_CREAT|O_CLOEXEC|O_WRONLY;

  if (name[1] == '>') {
#ifdef _WIN32
    ignore_result(asprintf(errmsg,
      "Windows does not support O_APPEND"));
    return false;
#else
    *flags |= O_APPEND;
    *name_p = name + 2;
#endif
  } else {
    *flags |= O_TRUNC;
    *name_p = name + 1;
  }

  return true;
}

watchman_trigger_command::watchman_trigger_command(
    const w_root_t* root,
    const json_ref& trig,
    char** errmsg)
    : definition(trig),
      envht(w_envp_make_ht()),
      append_files(false),
      stdin_style(input_dev_null),
      max_files_stdin(0),
      stdout_flags(0),
      stderr_flags(0),
      stdout_name(nullptr),
      stderr_name(nullptr),
      current_proc(0),
      ping_(w_event_make()) {
  auto queryDef =
      json_object({{"expression", definition.get_default("expression")}});
  auto relative_root = definition.get_default("relative_root");
  if (relative_root) {
    json_object_set_nocheck(queryDef, "relative_root", relative_root);
  }

  query = w_query_parse(root, queryDef, errmsg);
  if (!query) {
    return;
  }

  auto name = trig.get_default("name");
  if (!name || !json_is_string(name)) {
    *errmsg = strdup("invalid or missing name");
    return;
  }
  triggername = json_to_w_string(name);

  command = definition.get_default("command");
  if (!command || !json_is_array(command) || !json_array_size(command)) {
    *errmsg = strdup("invalid command array");
    return;
  }

  append_files = json_is_true(trig.get_default("append_files", json_false()));
  if (append_files) {
    // This is unfortunately a bit of a hack.  When appending files to the
    // command line we need a list of just the file names.  We would normally
    // just set the field list to contain the name, but that may conflict with
    // the setting for the "stdin" property that is managed below; if they
    // didn't ask for the name, we can't just force it in. As a bit of an
    // "easy" workaround, we'll capture the list of names from the deduping
    // mechanism.
    query->dedup_results = true;
  }

  auto ele = definition.get_default("stdin");
  if (!ele) {
    stdin_style = input_dev_null;
  } else if (json_is_array(ele)) {
    stdin_style = input_json;
    if (!parse_field_list(ele, &query->fieldList, errmsg)) {
      return;
    }
  } else if (json_is_string(ele)) {
    const char *str = json_string_value(ele);
    if (!strcmp(str, "/dev/null")) {
      stdin_style = input_dev_null;
    } else if (!strcmp(str, "NAME_PER_LINE")) {
      stdin_style = input_name_list;
      if (!parse_field_list(
              json_array({typed_string_to_json("name")}),
              &query->fieldList,
              errmsg)) {
        return;
      }
    } else {
      ignore_result(asprintf(errmsg, "invalid stdin value %s", str));
      return;
    }
  } else {
    *errmsg = strdup("invalid value for stdin");
    return;
  }

  // unlimited unless specified
  auto ival =
      json_integer_value(trig.get_default("max_files_stdin", json_integer(0)));
  if (ival < 0) {
    *errmsg = strdup("max_files_stdin must be >= 0");
    return;
  }
  max_files_stdin = ival;

  json_unpack(trig, "{s:s}", "stdout", &stdout_name);
  json_unpack(trig, "{s:s}", "stderr", &stderr_name);

  if (!parse_redirection(&stdout_name, &stdout_flags, "stdout", errmsg)) {
    return;
  }

  if (!parse_redirection(&stderr_name, &stderr_flags, "stderr", errmsg)) {
    return;
  }

  // Set some standard vars
  w_envp_set(envht, "WATCHMAN_ROOT", root->root_path);
  w_envp_set_cstring(envht, "WATCHMAN_SOCK", get_sock_name());
  w_envp_set(envht, "WATCHMAN_TRIGGER", triggername);
}

void watchman_trigger_command::stop() {
  stopTrigger_ = true;
  if (triggerThread_.joinable()) {
    w_event_set(ping_);
    triggerThread_.join();
  }
}

watchman_trigger_command::~watchman_trigger_command() {
  if (triggerThread_.joinable() && !stopTrigger_) {
    // We could try to call stop() here, but that is paving over the problem,
    // especially if we happen to be the triggerThread_ for some reason.
    watchman::log(
        watchman::FATAL, "destroying trigger without stopping it first\n");
  }
  w_event_destroy(ping_);
}

void watchman_trigger_command::start(const w_root_t* root) {
  subscriber_ =
      root->unilateralResponses->subscribe([this] { w_event_set(ping_); });
  triggerThread_ = std::thread([this, root] {
    try {
      run(root);
    } catch (const std::exception& e) {
      watchman::log(
          watchman::ERR, "exception in trigger thread: ", e.what(), "\n");
    }
  });
}

/* trigger /root triggername [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(struct watchman_client* client, const json_ref& args) {
  char *errmsg = NULL;
  bool need_save = true;
  struct unlocked_watchman_root unlocked;
  std::unique_ptr<watchman_trigger_command> cmd;
  json_ref trig;
  json_ref resp;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments");
    goto done;
  }

  trig = args.at(2);
  if (json_is_string(trig)) {
    trig = build_legacy_trigger(unlocked.root, client, args);
    if (!trig) {
      goto done;
    }
  }

  cmd = watchman::make_unique<watchman_trigger_command>(
      unlocked.root, trig, &errmsg);

  if (errmsg) {
    send_error_response(client, "%s", errmsg);
    goto done;
  }

  resp = make_response();
  resp.set("triggerid", w_string_to_json(cmd->triggername));

  {
    auto wlock = unlocked.root->triggers.wlock();
    auto& map = *wlock;
    auto& old = map[cmd->triggername];

    if (old && json_equal(cmd->definition, old->definition)) {
      // Same definition: we don't and shouldn't touch things, so that we
      // preserve the associated trigger clock and don't cause the trigger
      // to re-run immediately
      resp.set(
          "disposition",
          typed_string_to_json("already_defined", W_STRING_UNICODE));
      need_save = false;
    } else {
      resp.set(
          "disposition",
          typed_string_to_json(old ? "replaced" : "created", W_STRING_UNICODE));
      if (old) {
        // If we're replacing an old definition, be sure to stop the old
        // one before we destroy it, and before we start the new one.
        old->stop();
      }
      // Start the new trigger thread
      cmd->start(unlocked.root);
      old = std::move(cmd);

      need_save = true;
    }
  }

  if (need_save) {
    struct write_locked_watchman_root lock;

    w_root_lock(&unlocked, "trigger-add", &lock);
    // Force the trigger to be eligible to run now
    lock.root->inner.ticks++;
    w_root_unlock(&lock, &unlocked);

    w_state_save();
  }

  send_and_dispose_response(client, std::move(resp));

done:
  if (errmsg) {
    free(errmsg);
  }
  w_root_delref(&unlocked);
}
W_CMD_REG("trigger", cmd_trigger, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
