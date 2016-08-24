/* Finit state machine
 *
 * Copyright (c) 2016  Jonas Johansson <jonas.johansson@westermo.se>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <sys/types.h>

#include "finit.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "service.h"
#include "sig.h"
#include "tty.h"
#include "sm.h"

sm_t sm;

void sm_init(sm_t *sm)
{
	sm->state = SM_BOOTSTRAP_STATE;
	sm->newlevel = -1;
	sm->reload = 0;
	sm->in_teardown = 0;
}

static char *sm_status(sm_t *sm)
{
	switch (sm->state) {
	case SM_BOOTSTRAP_STATE:
		return "bootstrap";

	case SM_RUNNING_STATE:
		return "running";

	case SM_RUNLEVEL_CHANGE_STATE:
		return "runlevel/change";

	case SM_RUNLEVEL_WAIT_STATE:
		return "runlevel/wait";

	case SM_RELOAD_CHANGE_STATE:
		return "reload/change";

	case SM_RELOAD_WAIT_STATE:
		return "reload/wait";

	default:
		return "unknown";
	}
}

void sm_set_runlevel(sm_t *sm, int newlevel)
{
	sm->newlevel = newlevel;
}

void sm_set_reload(sm_t *sm)
{
	sm->reload = 1;
}

int sm_is_in_teardown(sm_t *sm)
{
	return sm->in_teardown;
}

void sm_step(sm_t *sm)
{
	sm_state_t old_state;

restart:
	old_state = sm->state;

	_d("state: %s", sm_status(sm));

	switch (sm->state) {
	case SM_BOOTSTRAP_STATE:
		_d("Bootstrapping all services in runlevel S from %s", FINIT_CONF);
		service_step_all(SVC_TYPE_RUN | SVC_TYPE_TASK | SVC_TYPE_SERVICE);
		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RUNNING_STATE:
		/* runlevel changed? */
		if (sm->newlevel >= 0 && sm->newlevel <= 9) {
			if (runlevel == sm->newlevel) {
				sm->newlevel = -1;
				break;
			}
			sm->state = SM_RUNLEVEL_CHANGE_STATE;
			break;
		}
		/* reload ? */
		if (sm->reload) {
			sm->reload = 0;
			sm->state = SM_RELOAD_CHANGE_STATE;
		}
		break;

	case SM_RUNLEVEL_CHANGE_STATE:
		prevlevel    = runlevel;
		runlevel     = sm->newlevel;
		sm->newlevel = -1;

		_d("Setting new runlevel --> %d <-- previous %d", runlevel, prevlevel);
		runlevel_set(prevlevel, runlevel);

		/* Make sure to (re)load all *.conf in /etc/finit.d/ */
		conf_reload_dynamic();

		_d("Stopping services services not allowed in new runlevel ...");
		sm->in_teardown = 1;
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RUNLEVEL_WAIT_STATE;
		break;

	case SM_RUNLEVEL_WAIT_STATE:
		/* Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later. */
		if (!service_stop_is_done())
			break;

		/* Prev runlevel services stopped, call hooks before starting new runlevel ... */
		_d("All services have been stoppped, calling runlevel change hooks ...");
		plugin_run_hooks(HOOK_RUNLEVEL_CHANGE);  /* Reconfigure HW/VLANs/etc here */

		_d("Starting services services new to this runlevel ...");
		sm->in_teardown = 0;
		service_step_all(SVC_TYPE_ANY);

		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

		/* Disable login in single-user mode as well as shutdown/reboot */
		if (runlevel == 1 || runlevel == 0 || runlevel == 6)
			touch("/etc/nologin");
		else
			erase("/etc/nologin");

		if (0 == runlevel) {
			do_shutdown(SHUT_OFF);
			sm->state = SM_RUNNING_STATE;
			break;
		}
		if (6 == runlevel) {
			do_shutdown(SHUT_REBOOT);
			sm->state = SM_RUNNING_STATE;
			break;
		}

		/* No TTYs run at bootstrap, they have a delayed start. */
		if (prevlevel > 0)
			tty_runlevel(runlevel);

		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RELOAD_CHANGE_STATE:
		/* First reload all *.conf in /etc/finit.d/ */
		conf_reload_dynamic();

		/* Then, mark all affected service conditions as in-flux and
		 * let all affected services move to WAITING/HALTED */
		_d("Stopping services services not allowed after reconf ...");
		sm->in_teardown = 1;
		cond_reload();
		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);

		sm->state = SM_RELOAD_WAIT_STATE;
		break;

	case SM_RELOAD_WAIT_STATE:
		/* Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later. */
		if (!service_stop_is_done())
			break;

		sm->in_teardown = 0;
		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

		_d("Starting services after reconf ...");
		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);

		_d("Calling reconf hooks ...");
		plugin_run_hooks(HOOK_SVC_RECONF);

		service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_INETD);
		_d("Reconfiguration done");

		sm->state = SM_RUNNING_STATE;
		break;
	}

	if (sm->state != old_state) {
		goto restart;
	}
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
