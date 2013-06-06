/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#define _GNU_SOURCE

#include "replayer.h"

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/poll.h>
#include <sys/ptrace.h>
#include <asm/ptrace-abi.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/user.h>
#include <unistd.h>

#include "dbg_gdb.h"
#include "rep_sched.h"
#include "rep_process_event.h"
#include "rep_process_signal.h"

#include "../share/dbg.h"
#include "../share/hpc.h"
#include "../share/trace.h"
#include "../share/ipc.h"
#include "../share/sys.h"
#include "../share/util.h"
#include "../share/wrap_syscalls.h"

#include <perfmon/pfmlib_perf_event.h>

#define SAMPLE_SIZE 		10
#define NUM_SAMPLE_PAGES 	1
/* sigh */
#define MAX_NUM_BREAKPOINTS     128

static const struct dbg_request continue_all_tasks = {
	.type = DREQ_CONTINUE,
	.target = -1,
	.params = { {0} }
};

/* FIXME use a real data structure */
static struct breakpoint {
	void* addr;
	byte overwritten_data;
} breakpoint_table[MAX_NUM_BREAKPOINTS];

static const byte int_3_insn = 0xCC;

static const struct flags* rr_flags;

/* Nonzero after the first exec() has been observed during replay.
 * After this point, the first recorded binary image has been exec()d
 * over the initial rr image. */
static bool validate = FALSE;

static void debug_memory(struct context* ctx)
{
	/* dump memory as user requested */
	if (rr_flags->dump_on == ctx->trace.stop_reason
	    || rr_flags->dump_on == DUMP_ON_ALL
	    || rr_flags->dump_at == ctx->trace.global_time) {
		char pid_str[PATH_MAX];
		snprintf(pid_str, sizeof(pid_str) - 1, "%s/%d_%d_rep",
			 get_trace_path(),
			 ctx->child_tid, ctx->trace.global_time);
		print_process_memory(ctx, pid_str);
	}

	/* check memory checksum */
	if (validate
	    && ((rr_flags->checksum == CHECKSUM_ALL)
		|| (rr_flags->checksum == CHECKSUM_SYSCALL
		    && ctx->trace.state == STATE_SYSCALL_EXIT)
		|| (rr_flags->checksum <= ctx->trace.global_time))) {
		validate_process_memory(ctx);
	}
}

static void replay_init_scratch_memory(struct context* ctx,
				       struct mmapped_file *file)
{
    /* initialize the scratchpad as the recorder did, but
     * make it PROT_NONE. The idea is just to reserve the
     * address space so the replayed process address map
     * looks like the recorded process, if it were to be
     * probed by madvise or some other means. But we make
     * it PROT_NONE so that rogue reads/writes to the
     * scratch memory are caught.
     */

    /* set up the mmap system call */
    struct user_regs_struct orig_regs;
    read_child_registers(ctx->child_tid, &orig_regs);

    struct user_regs_struct mmap_call = orig_regs;

    mmap_call.eax = SYS_mmap2;
    mmap_call.ebx = (uintptr_t)file->start;
    mmap_call.ecx = file->end - file->start;
    mmap_call.edx = PROT_NONE;
    mmap_call.esi = MAP_PRIVATE | MAP_ANONYMOUS;
    mmap_call.edi = -1;
    mmap_call.ebp = 0;

    inject_and_execute_syscall(ctx,&mmap_call);

    write_child_registers(ctx->child_tid,&orig_regs);
}

/**
 * Return the value of |reg| in |regs|, or set |*defined = 0| and
 * return an undefined value if |reg| isn't found.
 */
static long get_reg(const struct user_regs_struct* regs, dbg_register reg,
		    int* defined)
{
	*defined = 1;
	switch (reg) {
	case DREG_EAX: return regs->eax;
	case DREG_ECX: return regs->ecx;
	case DREG_EDX: return regs->edx;
	case DREG_EBX: return regs->ebx;
	case DREG_ESP: return regs->esp;
	case DREG_EBP: return regs->ebp;
	case DREG_ESI: return regs->esi;
	case DREG_EDI: return regs->edi;
	case DREG_EIP: return regs->eip;
	case DREG_EFLAGS: return regs->eflags;
	case DREG_CS: return regs->xcs;
	case DREG_SS: return regs->xss;
	case DREG_DS: return regs->xds;
	case DREG_ES: return regs->xes;
	case DREG_FS: return regs->xfs;
	case DREG_GS: return regs->xgs;
	case DREG_ORIG_EAX: return regs->orig_eax;
	default:
		*defined = 0;
		return 0;
	}
}

static dbg_threadid_t get_threadid(struct context* ctx)
{
	dbg_threadid_t thread = ctx->rec_tid;
	return thread;
}

static byte* read_mem(struct context* ctx, void* addr, size_t len)
{
	/* XXX will gdb ever make request for unreadable memory?  If
	 * so, we need to use read_child_data_checked() here. */
	return read_child_data_tid(ctx->child_tid, len, addr);
}

static struct breakpoint* find_breakpoint(void* addr)
{
	int i;
	for (i = 0; i < MAX_NUM_BREAKPOINTS; ++i) {
		struct breakpoint* bp = &breakpoint_table[i];
		if (addr == bp->addr) {
			return bp;
		}
	}
	return NULL;
}

static void set_sw_breakpoint(struct context *ctx,
			      const struct dbg_request* req)
{
	struct breakpoint* bp;
	byte* orig_data_ptr;

	assert(sizeof(int_3_insn) == req->params.mem.len);

	bp = find_breakpoint(NULL/* unallocated */);
	assert(bp && "Sorry, ran out of breakpoints");
	bp->addr = req->params.mem.addr;

	orig_data_ptr = read_child_data(ctx, 1, bp->addr);
	bp->overwritten_data = *orig_data_ptr;
	sys_free((void**)&orig_data_ptr);

	write_child_data_n(ctx->child_tid,
			   sizeof(int_3_insn), bp->addr, &int_3_insn);
}

static void remove_sw_breakpoint(struct context *ctx,
				 const struct dbg_request* req)
{
	struct breakpoint* bp;

	assert(sizeof(int_3_insn) == req->params.mem.len);

	bp = find_breakpoint(req->params.mem.addr);
	if (!bp) {
		warn("Couldn't find breakpoint %p to remove",
		     req->params.mem.addr);
		return;
	}
	write_child_data_n(ctx->child_tid,
			   sizeof(bp->overwritten_data), bp->addr,
			   &bp->overwritten_data);

	memset(bp, 0, sizeof(*bp));
}

static int eip_is_breakpoint(void* eip)
{
	void* ip = (void*)((uintptr_t)eip - sizeof(int_3_insn));
	return !!find_breakpoint(ip);
}

/* Reply to debugger requests until the debugger asks us to resume
 * execution. */
static struct dbg_request process_debugger_requests(struct dbg_context* dbg,
						    struct context* ctx)
{
	if (!dbg) {
		return continue_all_tasks;
	}
	while (1) {
		struct dbg_request req = dbg_get_request(dbg);
		if (dbg_is_resume_request(&req)) {
			return req;
		}

		switch (req.type) {
		case DREQ_GET_CURRENT_THREAD: {
			dbg_reply_get_current_thread(dbg, get_threadid(ctx));
			continue;
		}
		case DREQ_GET_IS_THREAD_ALIVE:
			dbg_reply_get_is_thread_alive(
				dbg, !!rep_sched_lookup_thread(req.target));
			continue;
		case DREQ_GET_MEM: {
			byte* mem = read_mem(ctx, req.params.mem.addr,
					     req.params.mem.len);
			dbg_reply_get_mem(dbg, mem);
			sys_free((void**)&mem);
			continue;
		}
		case DREQ_GET_OFFSETS:
			/* TODO */
			dbg_reply_get_offsets(dbg);
			continue;
		case DREQ_GET_REG: {
			struct user_regs_struct regs;
			dbg_regvalue_t val;

			read_child_registers(ctx->child_tid, &regs);
			val.value = get_reg(&regs, req.params.reg,
					    &val.defined);
			dbg_reply_get_reg(dbg, val);
			continue;
		}
		case DREQ_GET_REGS: {
			struct user_regs_struct regs;
			struct dbg_regfile file;
			int i;
			dbg_regvalue_t* val;

			read_child_registers(ctx->child_tid, &regs);
			memset(&file, 0, sizeof(file));
			for (i = DREG_EAX; i < DREG_NUM_USER_REGS; ++i) {
				val = &file.regs[i];
				val->value = get_reg(&regs, i, &val->defined);
			}
			val = &file.regs[DREG_ORIG_EAX];
			val->value = get_reg(&regs, DREG_ORIG_EAX,
					     &val->defined);

			dbg_reply_get_regs(dbg, &file);
			continue;
		}
		case DREQ_GET_STOP_REASON: {
			struct context* t =
				req.target > 0 ?
				rep_sched_lookup_thread(req.target) : ctx;
			dbg_reply_get_stop_reason(dbg,
						  t ? t->rec_tid : -1,
						  t ? t->child_sig : -1);
			continue;
		}
		case DREQ_GET_THREAD_LIST: {
			/* TODO */
			dbg_threadid_t list = get_threadid(ctx);
			dbg_reply_get_thread_list(dbg, &list, 1);
			continue;
		}
		case DREQ_INTERRUPT:
			/* Tell the debugger we stopped and await
			 * further instructions. */
			dbg_notify_stop(dbg, get_threadid(ctx), 0);
			continue;
		case DREQ_SET_SW_BREAK:
			set_sw_breakpoint(ctx, &req);
			dbg_reply_watchpoint_request(dbg, 0);
			continue;
		case DREQ_REMOVE_SW_BREAK:
			remove_sw_breakpoint(ctx, &req);
			dbg_reply_watchpoint_request(dbg, 0);
			break;
		case DREQ_REMOVE_HW_BREAK:
		case DREQ_REMOVE_RD_WATCH:
		case DREQ_REMOVE_WR_WATCH:
		case DREQ_REMOVE_RDWR_WATCH:
		case DREQ_SET_HW_BREAK:
		case DREQ_SET_RD_WATCH:
		case DREQ_SET_WR_WATCH:
		case DREQ_SET_RDWR_WATCH:
			dbg_reply_watchpoint_request(dbg, -1);
			continue;
		default:
			fatal("Unknown debugger request %d", req.type);
		}
	}
}

/**
 * Compares the register file as it appeared in the recording phase
 * with the current register file.
 *
 * XXX should this be a common helper?
 */
static void validate_args(int syscall, int state, struct context* ctx)
{
	struct user_regs_struct cur_reg;

	/* don't validate anything before execve is done as the actual
	 * process did not start prior to this point */
	if (!validate) {
		return;
	}
	read_child_registers(ctx->child_tid, &cur_reg);
	if (compare_register_files("syscall now", &cur_reg, "recorded",
				   &ctx->trace.recorded_regs, 1, 0)) {
		fatal("[syscall number %d, state %d, trace file line %d]\n",
		      syscall, state, get_trace_file_lines_counter());
	}
	/* TODO: add perf counter validations (hw int, page faults, insts) */
}

/**
 * Continue until reaching either the "entry" of an emulated syscall,
 * or the entry or exit of an executed syscall.  |emu| is nonzero when
 * we're emulating the syscall.  Return 0 when the next syscall
 * boundary is reached, or nonzero if advancing to the boundary was
 * interrupted by an unknown trap.
 */
static int cont_syscall_boundary(struct context* ctx, int emu, int stepi)
{
	pid_t tid = ctx->child_tid;

	assert(ctx->replay_sig == 0);

	if (emu) {
		if (stepi) {
			sys_ptrace_sysemu_singlestep(tid, ctx->replay_sig);
		} else {
			sys_ptrace_sysemu(tid);
		}
	} else {
		if (stepi) {
			sys_ptrace_singlestep(tid, ctx->replay_sig);
		} else {
			sys_ptrace_syscall_sig(tid, ctx->replay_sig);
		}
	}
	sys_waitpid(tid, &ctx->status);

	switch ((ctx->child_sig = signal_pending(ctx->status))) {
	case 0:
		break;
	case SIGCHLD:
		/* SIGCHLD is pending, do not deliver it, wait for it
		 * to appear in the trace SIGCHLD is the only signal
		 * that should ever be generated as all other signals
		 * are emulated! */
		return cont_syscall_boundary(ctx, emu, stepi);
	case SIGTRAP:
		return 1;
	default:
		log_err("Replay got unrecorded signal %d", ctx->child_sig);
		emergency_debug(ctx);
	}

	assert(ctx->child_sig == 0);

	/* XXX why is this here? */
	rep_child_buffer0(ctx);
	return 0;
}

/**
 *  Step over the system call instruction to "exit" the emulated
 *  syscall.
 */
static void step_exit_syscall_emu(struct context *ctx)
{
	pid_t tid = ctx->child_tid;
	struct user_regs_struct regs;

	assert(ctx->replay_sig == 0);

	read_child_registers(tid, &regs);

	sys_ptrace_sysemu_singlestep(tid, ctx->replay_sig);
	sys_waitpid(tid, &ctx->status);

	write_child_registers(tid, &regs);

	ctx->replay_sig = 0;
	ctx->status = 0;
}

/**
 * Advance to the next syscall entry (or virtual entry) according to
 * |step|.  Return 0 if successful, or nonzero if an unhandled trap
 * occurred.
 */
static int enter_syscall(struct context* ctx,
			 const struct rep_trace_step* step,
			 int stepi)
{
	int ret;
	if ((ret = cont_syscall_boundary(ctx, step->params.syscall.emu,
					 stepi))) {
		return ret;
	}
	validate_args(step->params.syscall.no, STATE_SYSCALL_ENTRY, ctx);
	return ret;
}

/**
 * Advance past the reti (or virtual reti) according to |step|.
 * Return 0 if successful, or nonzero if an unhandled trap occurred.
 */
static int exit_syscall(struct context* ctx,
			const struct rep_trace_step* step,
			int stepi)
{
	int i, emu = step->params.syscall.emu;

	if (!emu) {
		int ret = cont_syscall_boundary(ctx, emu, stepi);
		if (ret) {
			return ret;
		}
	}

	for (i = 0; i < step->params.syscall.num_emu_args; ++i) {
		set_child_data(ctx);
	}
	if (step->params.syscall.emu_ret) {
		set_return_value(ctx);
	}
	validate_args(step->params.syscall.no, STATE_SYSCALL_EXIT, ctx);

	if (emu) {
		/* XXX verify that this can't be interrupted by a
		 * breakpoint trap */
		step_exit_syscall_emu(ctx);
	}
	return 0;
}

/**
 * Try to execute |step|, adjusting for |req| if needed.  Return 0 if
 * |step| was made, or nonzero if there was a trap or |step| needs
 * more work.
 */
static int try_one_trace_step(struct context* ctx,
			      const struct rep_trace_step* step,
			      const struct dbg_request* req)
{
	int stepi = (DREQ_STEP == req->type
		     && get_threadid(ctx) == req->target);
	switch (step->action) {
	case TSTEP_RETIRE:
		return 0;
	case TSTEP_ENTER_SYSCALL:
		return enter_syscall(ctx, step, stepi);
	case TSTEP_EXIT_SYSCALL:
		return exit_syscall(ctx, step, stepi);
	default:
		fatal("Unhandled step type %d", step->action);
		return 0;
	}
}

static void replay_one_trace_frame(struct dbg_context* dbg,
				   struct context* ctx)
{
	struct dbg_request req;
	struct rep_trace_step step;

	/* Advance the trace until we've exec()'d the tracee before
	 * processing debugger requests.  Otherwise the debugger host
	 * will be confused about the initial executable image,
	 * rr's. */
	if (validate) {
		req = process_debugger_requests(dbg, ctx);
		assert(dbg_is_resume_request(&req));
	}

	/* print some kind of progress */
	if (ctx->trace.global_time % 10000 == 0) {
		fprintf(stderr, "time: %u\n",ctx->trace.global_time);
	}

	if (ctx->child_sig != 0) {
		assert(ctx->trace.stop_reason == -ctx->child_sig);
		ctx->child_sig = 0;
	}

	/* Ask the trace-interpretation code what to do next in order
	 * to retire the current frame. */
	memset(&step, 0, sizeof(step));
	if (ctx->trace.stop_reason == USR_INIT_SCRATCH_MEM) {
		/* for checksumming: make a note that this area is
		 * scratch and need not be validated. */
		struct mmapped_file file;
		read_next_mmapped_file_stats(&file);
		replay_init_scratch_memory(ctx, &file);
		add_scratch((void*)ctx->trace.recorded_regs.eax,
			    file.end - file.start);

		/* TODO */
		step.action = TSTEP_RETIRE;
	} else if (ctx->trace.stop_reason == USR_EXIT) {
		rep_sched_deregister_thread(&ctx);
		/* Early-return because |ctx| is gone now. */
		return;
	} else if (ctx->trace.stop_reason == USR_FLUSH) {
		rep_process_flush(ctx);

		/* TODO */
		step.action = TSTEP_RETIRE;
	} else if (ctx->trace.stop_reason < 0) {
		/* stop reason is a signal - use HPC */
		rep_process_signal(ctx, validate);

		/* TODO */
		step.action = TSTEP_RETIRE;
	} else {
		/* XXX not so pretty ... */
		validate |= (ctx->trace.state == STATE_SYSCALL_EXIT
			     && ctx->trace.stop_reason == SYS_execve);
		/* stop reason is a system call - can be done with
		 * ptrace */
		rep_process_syscall(ctx, rr_flags->redirect, &step);
	}

	/* XXX this pattern may not work ... it's simple so let's try
	 * it though */

	/* Advance until |step| has been fulfilled. */
	while (try_one_trace_step(ctx, &step, &req)) {
		assert(SIGTRAP == ctx->child_sig && "Unknown trap");

		/* Currently we only understand software breakpoints
		 * and successful stepi's.  The response in both cases
		 * is the same, so just make sure we saw an action we
		 * were expecting.  */
		assert(eip_is_breakpoint((void*)read_child_eip(ctx->child_tid))
		       || (DREQ_STEP == req.type
			   && req.target == get_threadid(ctx)));

		/* Notify the debugger and process any new requests
		 * that might have triggered before resuming. */
		dbg_notify_stop(dbg, get_threadid(ctx),	0x05/*gdb mandate*/);
		req = process_debugger_requests(dbg, ctx);
		assert(dbg_is_resume_request(&req));
	}

	/* Every time a non-wrapped event happens, the hpc is
	 * reset. When an event that requires hpc occurs, we
	 * read the hpc at that point and reset the hpc
	 * interval to the required rbc minus the current hpc.
	 * All this happens since the wrapped event do not
	 * reset the hpc,therefore the previous techniques of
	 * starting the hpc only the at the previous event to
	 * the one that requires it, doesn't work, since the
	 * previous event may be a wrapped syscall.
	 *
	 * XXX clarify
	 */
	if (ctx->trace.stop_reason != USR_FLUSH) {
		reset_hpc(ctx, 0);
	}
	debug_memory(ctx);
}

static void check_initial_register_file()
{
	rep_sched_get_thread();
}

void replay(struct flags flags)
{
	struct dbg_context* dbg = NULL;

	rr_flags = &flags;

	if (!rr_flags->autopilot) {
		dbg = dbg_await_client_connection("127.0.0.1",
						  rr_flags->dbgport);
	}

	check_initial_register_file();

	while (rep_sched_get_num_threads()) {
		replay_one_trace_frame(dbg, rep_sched_get_thread());
	}

	if (dbg) {
		/* TODO keep record of the code, if it's useful */
		dbg_notify_exit_code(dbg, 0);
	}

	log_info("Replayer successfully finished.");
	fflush(stdout);

	dbg_destroy_context(&dbg);
}

void emergency_debug(struct context* ctx)
{
	struct dbg_context* dbg = dbg_await_client_connection("127.0.0.1",
							      ctx->child_tid);
	process_debugger_requests(dbg, ctx);
	fatal("Can't resume execution from invalid state");
}
