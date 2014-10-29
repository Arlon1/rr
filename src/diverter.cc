/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Diverter"

#include "diverter.h"

#include "debugger_gdb.h"
#include "DiversionSession.h"
#include "log.h"
#include "ReplaySession.h"
#include "task.h"

// The global diversion session, of which there can only be one at a
// time currently.  See long comment at the top of diverter.h.
static DiversionSession::shr_ptr session;
// Number of client references to this, if it's a diversion
// session.  When there are 0 refs this is considered to be
// dying.
static int diversion_refcount;

/**
 * Process debugger requests made through |dbg| until action needs to
 * be taken by the caller (a resume-execution request is received).
 * The returned Task* is the target of the resume-execution request.
 *
 * The received request is returned through |req|.
 */
static Task* process_debugger_requests(struct dbg_context* dbg, Task* t,
                                       struct dbg_request* req) {
  while (true) {
    *req = dbg_get_request(dbg);

    if (dbg_is_resume_request(req)) {
      if (diversion_refcount == 0) {
        return nullptr;
      }
      return t;
    }

    switch (req->type) {
      case DREQ_RESTART:
        return nullptr;

      case DREQ_READ_SIGINFO: {
        LOG(debug) << "Adding ref to diversion session";
        ++diversion_refcount;
        // TODO: maybe share with replayer.cc?
        uint8_t si_bytes[req->mem.len];
        memset(si_bytes, 0, sizeof(si_bytes));
        dbg_reply_read_siginfo(dbg, si_bytes, sizeof(si_bytes));
        continue;
      }
      case DREQ_SET_QUERY_THREAD: {
        Task* next_task = t->session().find_task(req->target.tid);
        t = next_task ? next_task : t;
        break;
      }
      case DREQ_WRITE_SIGINFO:
        LOG(debug) << "Removing reference to diversion session ...";
        assert(diversion_refcount > 0);
        --diversion_refcount;
        if (diversion_refcount == 0) {
          LOG(debug) << "  ... dying at next continue request";
        }
        dbg_reply_write_siginfo(dbg);
        continue;

      case DREQ_REMOVE_SW_BREAK:
      case DREQ_REMOVE_HW_BREAK:
      case DREQ_REMOVE_RD_WATCH:
      case DREQ_REMOVE_WR_WATCH:
      case DREQ_REMOVE_RDWR_WATCH:
      case DREQ_SET_SW_BREAK:
      case DREQ_SET_HW_BREAK:
      case DREQ_SET_RD_WATCH:
      case DREQ_SET_WR_WATCH:
      case DREQ_SET_RDWR_WATCH: {
        // Setting breakpoints in a dying diversion is assumed
        // to be a user action intended for the replay
        // session, so return to it now.
        if (diversion_refcount == 0) {
          return nullptr;
        }
        break;
      }

      default:
        break;
    }

    dispatch_debugger_request(*session, dbg, t, *req);
  }
}

static dbg_threadid_t get_threadid(Task* t) {
  dbg_threadid_t thread;
  thread.pid = t->tgid();
  thread.tid = t->rec_tid;
  return thread;
}

void divert(ReplaySession& replay, struct dbg_context* dbg, pid_t task,
            struct dbg_request* req) {
  LOG(debug) << "Starting debugging diversion for " << &replay;
  assert(!session && diversion_refcount == 0);

  session = replay.clone_diversion();
  diversion_refcount = 1;

  Task* t = session->find_task(task);
  while (true) {
    if (!(t = process_debugger_requests(dbg, t, req))) {
      break;
    }

    ReplaySession::RunCommand command =
        (DREQ_STEP == req->type && get_threadid(t) == req->target)
            ? Session::RUN_SINGLESTEP
            : Session::RUN_CONTINUE;
    auto result = session->diversion_step(t, command);

    if (result.status == DiversionSession::DIVERSION_EXITED) {
      diversion_refcount = 0;
      dbg_notify_exit_code(dbg, 0);
      break;
    }

    assert(result.status == DiversionSession::DIVERSION_CONTINUE);
    if (result.break_status.reason == Session::BREAK_NONE) {
      continue;
    }

    int sig = SIGTRAP;
    remote_ptr<void> watch_addr = nullptr;
    switch (result.break_status.reason) {
      case Session::BREAK_SIGNAL:
        sig = result.break_status.signal;
        break;
      case Session::BREAK_WATCHPOINT:
        watch_addr = result.break_status.watch_address;
        break;
      default:
        break;
    }
    /* Notify the debugger and process any new requests
     * that might have triggered before resuming. */
    dbg_notify_stop(dbg, get_threadid(result.break_status.task), sig,
                    watch_addr.as_int());
  }

  LOG(debug) << "... ending debugging diversion";
  assert(diversion_refcount == 0);
  session->kill_all_tasks();
  session = nullptr;
}
