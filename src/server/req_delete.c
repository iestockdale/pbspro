/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file	req_delete.c
 *
 * Functions relating to the Delete Job Batch Requests.
 *
 * Included funtions are:
 *	remove_stagein()
 *	acct_del_write()
 *	check_deletehistoryjob()
 *	issue_delete()
 *	req_deletejob()
 *	req_deletejob2()
 *	req_deleteReservation()
 *	post_delete_route()
 *	post_deljobfromresv_req()
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include "portability.h"
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "work_task.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "resv_node.h"
#include "queue.h"
#include "hook.h"

#ifdef WIN32
#include <windows.h>
#include "win.h"
#endif

#include "job.h"
#include "reservation.h"
#include "pbs_error.h"
#include "acct.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"


/* Global Data Items: */

extern char *msg_deletejob;
extern char *msg_delrunjobsig;
extern char *msg_manager;
extern char *msg_noDeljobfromResv;
extern char *msg_deleteresv;
extern char *msg_deleteresvJ;
extern char *msg_job_history_delete;
extern char *msg_job_history_notset;
extern char *msg_also_deleted_job_history;
extern char *msg_err_malloc;
extern struct server server;
extern time_t time_now;

/* External functions */

extern int issue_to_svr(char *, struct batch_request *, void (*func)(struct work_task *));
extern struct batch_request *cpy_stage(struct batch_request *, job *, enum job_atr, int);
extern resc_resv  *chk_rescResv_request(char *, struct batch_request *);

/* Private Functions in this file */

static void post_delete_route(struct work_task *);
static void post_delete_mom1(struct work_task *);
static void post_deljobfromresv_req(struct work_task *);
static void req_deletejob2(struct batch_request *preq, job *pjob);

/* Private Data Items */

static char *sigk  = "SIGKILL";
static char *sigt  = "SIGTERM";
static char *sigtj =  SIG_TermJob;
static char *acct_fmt = "requestor=%s@%s";
static int qdel_mail = 1; /* true: sending mail */


/**
 * @brief
 * 		remove_stagein() - request that mom delete staged-in files for a job
 *		used when the job is to be purged after files have been staged in
 *
 * @param[in,out]	pjob	- job
 */

void
remove_stagein(pjob)
job *pjob;
{
	struct batch_request *preq = 0;

	preq = cpy_stage(preq, pjob, JOB_ATR_stagein, 0);

	if (preq) { /* have files to delete		*/

		/* change the request type from copy to delete  */

		preq->rq_type = PBS_BATCH_DelFiles;
		preq->rq_extra = NULL;
		if (relay_to_mom(pjob, preq, release_req) == 0) {
			pjob->ji_qs.ji_svrflags &= ~JOB_SVFLG_StagedIn;
		} else {
			/* log that we were unable to remove the files */
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_FILE,
				LOG_NOTICE, pjob->ji_qs.ji_jobid,
				"unable to remove staged-in files for job");
			free_br(preq);
		}
	}
}

/**
 * @brief
 * 		acct_del_write - write the Job Deleted account record
 *
 * @param[in]	jid	- Job Id.
 * @param[in]	pjob	- Job structure.
 * @param[in]	preq - batch_request
 * @param[in]	nomail	- do not send mail to the job owner if enabled.
 */

static void
acct_del_write(char *jid, job *pjob, struct batch_request *preq, int nomail)
{
	(void) sprintf(log_buffer, acct_fmt, preq->rq_user, preq->rq_host);
	write_account_record(PBS_ACCT_DEL, jid, log_buffer);

	(void) sprintf(log_buffer, msg_manager, msg_deletejob,
		preq->rq_user, preq->rq_host);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO, jid, log_buffer);

	if (pjob != NULL) {
		int jt = is_job_array(jid);

		switch (jt) {
			case IS_ARRAY_NO: /* not a subjob */
			case IS_ARRAY_ArrayJob:

				/* if block set, send word */
				check_block(pjob, log_buffer);
		}

		if (preq->rq_parentbr == NULL && nomail == 0 &&
			svr_chk_owner(preq, pjob) != 0 &&
			qdel_mail != 0) {
			svr_mailowner_id(jid, pjob,
				MAIL_OTHER, MAIL_FORCE, log_buffer);
		}
	}
}

/**
 * @Brief
 *		If the job is a history job then purge its history
 *		If the job is a non-history job then it must be terminated before purging its history. Will be
 *		done by req_deletejob()
 *
 * @param[in]	preq	- Batch request structure.
 *
 * @return	int
 * @retval	TRUE	- Job history  has been purged
 * @retval	FALSE	- Job is not a history job
 */
int
check_deletehistoryjob(struct batch_request * preq)
{
	job *histpjob;
	job *pjob;
	int historyjob;
	int histerr;
	int t;
	char *jid;
	jid = preq->rq_ind.rq_delete.rq_objname;

	/*
	 * If the array subjob or range of subjobs are in a history state then
	 * reject the request as we cant delete history of array subjobs
	 */
	t = is_job_array(jid);
	if ((t == IS_ARRAY_Single) || (t == IS_ARRAY_Range)) {
		pjob = find_arrayparent(jid);
		if ((histerr = svr_chk_histjob(pjob))) {
			req_reject(PBSE_NOHISTARRAYSUBJOB, 0, preq);
			return TRUE;
		} else {
			/*
			 * Job is in a Non Finished state . It must be terminated and then its history
			 *  should be purged .
			 */
			return FALSE;
		}
	}

	histpjob = find_job(jid);

	historyjob = svr_chk_histjob(histpjob);
	if (historyjob == PBSE_HISTJOBID) {
		snprintf(log_buffer, sizeof(log_buffer),
			msg_job_history_delete, preq->rq_user,
			preq->rq_host);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
			jid,
			log_buffer);

		/* Issue history job delete request to remote server if job is moved. */
		if (histpjob->ji_qs.ji_state == JOB_STATE_MOVED)
			issue_delete(histpjob);

		if (histpjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) {
			if (histpjob->ji_ajtrk) {
				int i;
				for (i = 0; i < histpjob->ji_ajtrk->tkm_ct; i++) {
					char *sjid = mk_subjob_id(histpjob, i);
					job  *psjob;

					if ((psjob = find_job(sjid))) {
						snprintf(log_buffer, sizeof(log_buffer),
							msg_job_history_delete, preq->rq_user,
							preq->rq_host);
						log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
							sjid,
							log_buffer);

						job_purge(psjob);
					}
				}
			}
		}

		job_purge(histpjob);

		preq->rq_reply.brp_code = PBSE_HISTJOBDELETED;
		reply_send(preq);
		return TRUE;
	} else {
		/*
		 *  Job is in a Non Finished state . It must be terminated and then its history
		 * should be purged .
		 */
		return FALSE;
	}
}

/**
 * @Brief
 *		Issue PBS_BATCH_DeleteJob request to remote server.
 *
 * @param[in]	pjob - Job structure.
 */
void
issue_delete(job *pjob)
{
	struct batch_request   *preq;
	char rmt_server[PBS_MAXSERVERNAME + 1] = {'\0'};
	char *at = NULL;

	if (pjob == NULL)
		return;

	if ((at = strchr(pjob->ji_wattr[JOB_ATR_in_queue].at_val.at_str, (int)'@')) == NULL)
		return;

	snprintf(rmt_server, sizeof(rmt_server), "%s", at + 1);

	preq = alloc_br(PBS_BATCH_DeleteJob);
	if (preq == NULL)
		return;

	(void)strncpy(preq->rq_ind.rq_delete.rq_objname, pjob->ji_qs.ji_jobid, sizeof(preq->rq_ind.rq_delete.rq_objname) - 1);
	preq->rq_ind.rq_delete.rq_objname[sizeof(preq->rq_ind.rq_delete.rq_objname) - 1] = '\0';
	preq->rq_extend = malloc(strlen(DELETEHISTORY) + 1);
	if (preq->rq_extend == NULL) {
		log_err(errno, "issue_delete", msg_err_malloc);
		return;
	}

	(void)strncpy(preq->rq_extend, DELETEHISTORY, strlen(DELETEHISTORY) + 1);

	(void)issue_to_svr(rmt_server, preq, release_req);
}

/**
 * @brief
 * 		req_deletejob - service the Delete Job Request
 *
 *		This request deletes a job.
 *
 * @param[in]	preq	- Job Request
 */

void
req_deletejob(struct batch_request *preq)
{
	int forcedel = 0;
	int i;
	int j;
	char *jid;
	char *jidsj;
	int jt; /* job type */
	int offset;
	char *pc;
	job *pjob;
	job *parent;
	char *range;
	int sjst; /* subjob state */
	int x, y, z;
	int rc = 0;
	int delhist = 0;
	int maxindex = 0;
	int count = 0;

	jid = preq->rq_ind.rq_delete.rq_objname;

	if (preq->rq_extend && strstr(preq->rq_extend, DELETEHISTORY))
		delhist = 1;
	if (preq->rq_extend && strstr(preq->rq_extend, FORCEDEL))
		forcedel = 1;
	/* with nomail , nomail_force , nomail_deletehist or nomailforce_deletehist options are set
	 *  no mail is sent
	 */
	if (preq->rq_extend && strstr(preq->rq_extend, NOMAIL))
		qdel_mail = 0;
	else
		qdel_mail = 1;

	parent = chk_job_request(jid, preq, &jt);
	if (parent == NULL)
		return; /* note, req_reject already called */

	if (delhist) {
		rc = check_deletehistoryjob(preq);
		if (rc == TRUE)
			return;
	}


	if (jt == IS_ARRAY_NO) {

		/* just a regular job, pass it on down the line and be done
		 * If the request is to purge the history of the job then set ji_deletehistory to 1
		 */
		if (delhist)
			parent->ji_deletehistory = 1;
		req_deletejob2(preq, parent);
		return;

	} else if (jt == IS_ARRAY_Single) {

		/* single subjob, if running do full delete, */
		/* if not then just set it expired		 */

		offset = subjob_index_to_offset(parent, get_index_from_jid(jid));
		if (offset == -1) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}
		i = get_subjob_state(parent, offset);
		if (i == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		}

		if ((i == JOB_STATE_EXITING) && (forcedel == 0)) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		} else if (i == JOB_STATE_EXPIRED) {
			req_reject(PBSE_NOHISTARRAYSUBJOB, 0, preq);
			return;
		} else if ((pjob = find_job(jid)) != NULL) {
			/*
			 * If the request is to also purge the history of the sub job then set ji_deletehistory to 1
			 */
			if (delhist)
				pjob->ji_deletehistory = 1;
			req_deletejob2(preq, pjob);
			if (parent->ji_ajtrk)
				if (pjob->ji_terminated)
					parent->ji_ajtrk->tkm_dsubjsct++;
		} else {
			acct_del_write(jid, parent, preq, 0);
			parent->ji_ajtrk->tkm_tbl[offset].trk_substate =
				JOB_SUBSTATE_TERMINATED;
			set_subjob_tblstate(parent, offset, JOB_STATE_EXPIRED);
			parent->ji_ajtrk->tkm_dsubjsct++;

			reply_ack(preq);
		}
		chk_array_doneness(parent);
		return;

	} else if (jt == IS_ARRAY_ArrayJob) {
		/*
		 * For array jobs the history is stored at the parent array level and also at the subjob level .
		 * If the request is to delete the history of an array job then set  ji_deletehistory to 1 for
		 * the parent array.The function chk_array_doneness() will take care of eventually
		 *  purging the history .
		 */
		if (delhist)
			parent->ji_deletehistory = 1;
		/* The Array Job itself ... */
		/* for each subjob that is running, delete it via req_deletejob2 */

		++preq->rq_refct;

		/* keep the array from being removed while we are looking at it */
		parent->ji_ajtrk->tkm_flags |= TKMFLG_NO_DELETE;
		for (i = 0; i < parent->ji_ajtrk->tkm_ct; i++) {
			sjst = get_subjob_state(parent, i);
			if ((sjst == JOB_STATE_EXITING) && !forcedel)
				continue;
			if ((pjob = find_job(mk_subjob_id(parent, i)))) {
				if (delhist)
					pjob->ji_deletehistory = 1;
				if (pjob->ji_qs.ji_state == JOB_STATE_EXPIRED) {
					snprintf(log_buffer, sizeof(log_buffer),
						msg_job_history_delete, preq->rq_user,
						preq->rq_host);
					log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
						pjob->ji_qs.ji_jobid,
						log_buffer);
					job_purge(pjob);
				}else
					dup_br_for_subjob(preq, pjob, req_deletejob2);
			} else {
				/* Queued, Waiting, Held, just set to expired */
				parent->ji_ajtrk->tkm_tbl[i].trk_substate =
					JOB_SUBSTATE_TERMINATED;
				set_subjob_tblstate(parent, i, JOB_STATE_EXPIRED);
			}
		}
		parent->ji_ajtrk->tkm_flags &= ~TKMFLG_NO_DELETE;


		/* if deleting running subjobs, then just return;            */
		/* parent will be deleted when last running subjob(s) ends   */
		/* and reply will be sent to client when last delete is done */
		/* If not deleteing running subjobs, delete2 to del parent   */

		if (--preq->rq_refct == 0) {
			if ((parent = find_job(jid)) != NULL)
				req_deletejob2(preq, parent);
			else
				reply_send(preq);
		} else
			acct_del_write(jid, parent, preq, 0);

		return;
	}
	/* what's left to handle is a range of subjobs, foreach subjob 	*/
	/* if running, do full delete, else just update state	        */

	range = get_index_from_jid(jid);
	if (range == NULL) {
		req_reject(PBSE_IVALREQ, 0, preq);
		return;
	}

	++preq->rq_refct;
	while (1) {
		if ((i = parse_subjob_index(range, &pc, &x, &y, &z, &j)) == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			break;
		} else if (i == 1)
			break;
		/* Ensure that the range specified in the delete job request does not exceed the
		 index of the highest numbered array subjob */

		count = parent->ji_ajtrk->tkm_ct;
		maxindex = parent->ji_ajtrk->tkm_tbl[count-1].trk_index;
		if (x > maxindex) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			break;
		}
		while (x <= y) {
			i = numindex_to_offset(parent, x);
			if (i < 0) {
				x += z; /* no such index, ignore it */
				continue;
			}

			jidsj = mk_subjob_id(parent, i);
			if ((j = get_subjob_state(parent, i)) == JOB_STATE_RUNNING) {
				pjob = find_job(jidsj);
				if (pjob) {
					if (delhist)
						pjob->ji_deletehistory = 1;
					dup_br_for_subjob(preq, pjob, req_deletejob2);
				}
			} else if ((j != JOB_STATE_EXITING) || (forcedel == 1)) {
				/* not running, just set to expired */
				if (j == JOB_STATE_EXITING) {
					pjob = find_job(jidsj); /* subjob */
					if (pjob) {
						if (delhist)
							pjob->ji_deletehistory = 1;
						discard_job(pjob, "Forced Delete", 1);
						rel_resc(pjob);
						job_purge(pjob);
					}
				}
				parent->ji_ajtrk->tkm_tbl[i].trk_substate =
					JOB_SUBSTATE_TERMINATED;
				parent->ji_ajtrk->tkm_dsubjsct++;
				set_subjob_tblstate(parent, i, JOB_STATE_EXPIRED);
				acct_del_write(jidsj, NULL, preq, 1); /* no mail */
			}
			x += z;
		}
		range = pc;
	}
	if (i != -1) {
		(void) sprintf(log_buffer, msg_manager, msg_deletejob,
			preq->rq_user, preq->rq_host);
		if (qdel_mail != 0) {
			svr_mailowner_id(jid, parent, MAIL_OTHER, MAIL_FORCE, log_buffer);
		}
	}

	/* if deleting running subjobs, then just return;            */
	/* parent will be deleted when last running subjob(s) ends   */
	/* and reply will be sent to client when last delete is done */

	if (--preq->rq_refct == 0) {
		reply_send(preq);
		chk_array_doneness(parent);
	}

	return;
}
/**
 * @brief
 * 		req_deletejob2 - service the Delete Job Request
 *
 *		This request deletes a job.
 *
 * @param[in]	preq	- Job Request
 * @param[in,out]	pjob	- Job structure
 */

static void
req_deletejob2(struct batch_request *preq, job *pjob)
{
	int abortjob = 0;
	char *sig;
	int forcedel = 0;
	struct work_task *pwtold;
	struct work_task *pwtnew;
	struct batch_request *temp_preq = NULL;
	int rc;
	int is_mgr = 0;

	/* + 2 is for the '@' in user@host and for the null termination byte. */
	char by_user[PBS_MAXUSER + PBS_MAXHOSTNAME + 2] = {'\0'};

	/* active job is being deleted by delete job batch request */
	pjob->ji_terminated = 1;
	if ((preq->rq_user != NULL) && (preq->rq_host != NULL)) {
		sprintf(by_user, "%s@%s", preq->rq_user, preq->rq_host);
	}

	if ((preq->rq_extend && strstr(preq->rq_extend, FORCEDEL)))
		forcedel = 1;

	/* See if the request is coming from a manager */
	if (preq->rq_perm & (ATR_DFLAG_MGRD | ATR_DFLAG_MGWR))
		is_mgr = 1;

	if (pjob->ji_qs.ji_state == JOB_STATE_TRANSIT) {

		/*
		 * Find pid of router from existing work task entry,
		 * then establish another work task on same child.
		 * Next, signal the router and wait for its completion;
		 */

		pwtold = (struct work_task *) GET_NEXT(pjob->ji_svrtask);
		while (pwtold) {
			if ((pwtold->wt_type == WORK_Deferred_Child) ||
				(pwtold->wt_type == WORK_Deferred_Cmp)) {
				pwtnew = set_task(pwtold->wt_type,
					pwtold->wt_event, post_delete_route,
					preq);
				if (pwtnew) {

					/*
					 * reset type in case the SIGCHLD came
					 * in during the set_task;  it makes
					 * sure that next_task() will find the
					 * new entry.
					 */
					pwtnew->wt_type = pwtold->wt_type;
					pwtnew->wt_aux = pwtold->wt_aux;

#ifdef WIN32
					kill((HANDLE) pwtold->wt_event, SIGTERM);
#else
					kill((pid_t) pwtold->wt_event, SIGTERM);
#endif
					pjob->ji_qs.ji_substate = JOB_SUBSTATE_ABORT;
					return; /* all done for now */

				} else {

					req_reject(PBSE_SYSTEM, 0, preq);
					return;
				}
			}
			pwtold = (struct work_task *) GET_NEXT(pwtold->wt_linkobj);
		}
		/* should never get here ...  */
		log_err(-1, "req_delete", "Did not find work task for router");
		req_reject(PBSE_INTERNAL, 0, preq);
		return;

	} else if ((pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN) && (forcedel == 0)) {

		/* being sent to MOM, wait till she gets it going */
		/* retry in one second				  */

		pwtnew = set_task(WORK_Timed, time_now + 1, post_delete_route,
			preq);
		if (pwtnew == 0)
			req_reject(PBSE_SYSTEM, 0, preq);

		return;
	}

	if (is_mgr && forcedel) {
		/*
		 * Set exit status for the job to SIGKILL as we will not be working with any obit.
		 */
		pjob->ji_qs.ji_un.ji_exect.ji_exitstat = SIGKILL + 0x100;
	}

	if ((pjob->ji_qs.ji_state == JOB_STATE_RUNNING) ||
		(pjob->ji_qs.ji_substate == JOB_SUBSTATE_TERM)) {

		if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_RERUN) {
			/* rerun just started, clear that substate and */
			/* normal delete will happen when mom replies  */

			pjob->ji_qs.ji_substate = JOB_SUBSTATE_RUNNING;
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid, "deleting instead of reruning");
			acct_del_write(pjob->ji_qs.ji_jobid, pjob, preq, 0);
			reply_ack(preq);
			return;
		}

		if (((pjob->ji_qs.ji_substate == JOB_SUBSTATE_SUSPEND) ||
			(pjob->ji_qs.ji_substate == JOB_SUBSTATE_SCHSUSP)) &&
			(pjob->ji_wattr[(int) JOB_ATR_resc_released].at_flags & ATR_VFLAG_SET)) {
			set_resc_assigned(pjob, 0, INCR);
			job_attr_def[(int) JOB_ATR_resc_released].at_free(
					&pjob->ji_wattr[(int) JOB_ATR_resc_released]);
			pjob->ji_wattr[(int) JOB_ATR_resc_released].at_flags &= ~ATR_VFLAG_SET;
			if (pjob->ji_wattr[(int) JOB_ATR_resc_released_list].at_flags & ATR_VFLAG_SET) {
				job_attr_def[(int) JOB_ATR_resc_released_list].at_free(
						&pjob->ji_wattr[(int) JOB_ATR_resc_released_list]);
				pjob->ji_wattr[(int) JOB_ATR_resc_released_list].at_flags &= ~ATR_VFLAG_SET;
			}
		}


		if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PROVISION) {
			if (forcedel) {
				/*
				 * discard_job not called since job not sent
				 * to MOM
				 */
				log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
					LOG_INFO,
					pjob->ji_qs.ji_jobid, "deleting job");
				acct_del_write(pjob->ji_qs.ji_jobid, pjob,
					preq, 0);
				reply_ack(preq);
				rel_resc(pjob);
				(void) job_abt(pjob, NULL);
			} else
				req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}

		/*
		 * Job is in fact running, so we want to terminate it.
		 *
		 * Send signal request to MOM.  The server will automagically
		 * pick up and "finish" off the client request when MOM replies.
		 * If not "force" send special termjob signal,
		 * if "force" send SIGTERM.
		 */
		if (forcedel)
			sig = sigk;
		else
			sig = sigtj;

		if (is_mgr && forcedel)
			temp_preq = NULL;
		else
			temp_preq = preq;

		rc = issue_signal(pjob, sig, post_delete_mom1, temp_preq);

		/*
		 * If forcedel is set and request is from a manager,
		 * job is deleted from server regardless
		 * of issue_signal to MoM was a success or failure.
		 * Eventually, when the mom updates server about the job,
		 * server sends a discard message to mom and job is then
		 * deleted from mom as well.
		 */
		if ((rc || is_mgr) && forcedel) {
			(void) svr_setjobstate(pjob, JOB_STATE_EXITING,
				JOB_SUBSTATE_EXITED);
			if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_HERE) == 0)
				issue_track(pjob);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid, "Delete forced");
			acct_del_write(pjob->ji_qs.ji_jobid, pjob, preq, 0);
			reply_ack(preq);
			discard_job(pjob, "Forced Delete", 1);
			rel_resc(pjob);

			if (is_mgr) {
				/*
				 * Set exit status for the job to SIGKILL as we will not be working with any obit.
				 */
				pjob->ji_wattr[(int)JOB_ATR_exit_status].at_val.at_long = pjob->ji_qs.ji_un.ji_exect.ji_exitstat;
				pjob->ji_wattr[(int)JOB_ATR_exit_status].at_flags = ATR_VFLAG_SET | ATR_VFLAG_MODCACHE;
			}
			/*
			 * Check if the history of the finished job can be saved or it needs to be purged .
			 */
			svr_saveorpurge_finjobhist(pjob);
			return;
		}
		if (rc) {
			req_reject(rc, 0, preq); /* cant send to MOM */
			(void) sprintf(log_buffer, "Delete failed %d", rc);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_NOTICE,
				pjob->ji_qs.ji_jobid, log_buffer);
			return;
		}
		/* normally will ack reply when mom responds */
		update_job_finish_comment(pjob, JOB_SUBSTATE_TERMINATED, by_user);
		(void) sprintf(log_buffer, msg_delrunjobsig, sig);
		log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
			pjob->ji_qs.ji_jobid, log_buffer);
		return;
	} else if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_CHKPT) != 0) {

		/* job has restart file at mom, do end job processing */

		(void) svr_setjobstate(pjob, JOB_STATE_EXITING,
			JOB_SUBSTATE_EXITING);
		pjob->ji_momhandle = -1; /* force new connection */
		pjob->ji_mom_prot = PROT_INVALID;
		(void) set_task(WORK_Immed, 0, on_job_exit, (void *) pjob);

	} else if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_StagedIn) != 0) {

		/* job has staged-in file, should remove them */
		remove_stagein(pjob);
		abortjob = 1; /* set flag to abort job after mail sent */

	} else {

		/*
		 * the job is not transitting (though it may have been) and
		 * is not running, so abort it.
		 */

		abortjob = 1; /* set flag to abort job after mail sent */
	}
	/*
	 * Log delete and if requesting client is not job owner, send mail.
	 */

	acct_del_write(pjob->ji_qs.ji_jobid, pjob, preq, 0);

	if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) && !forcedel)
		chk_array_doneness(pjob);
	else if (abortjob) {
		if (pjob->ji_qs.ji_state == JOB_STATE_EXITING)
			discard_job(pjob, "Forced Delete", 1);
		rel_resc(pjob);
		(void) job_abt(pjob, NULL);
	}

	reply_send(preq);
}

/**
 * @brief
 * 		req_reservationOccurrenceEnd - service the PBS_BATCH_ResvOccurEnd Request
 *
 *		This request runs a hook script at the end of the reservation occurrence
 *
 * @param[in]	preq	- Job Request
 *
 */


void req_reservationOccurrenceEnd(struct batch_request *preq)
{
	char hook_msg[HOOK_MSG_SIZE] = {0};

        switch (process_hooks(preq, hook_msg, sizeof(hook_msg), pbs_python_set_interrupt)) {
		case 0:	/* explicit reject */
			reply_text(preq, PBSE_HOOKERROR, hook_msg);
			break;
		case 1: /* no recreate request as there are only read permissions */ 
		case 2:	/* no hook script executed - go ahead and accept event*/
			reply_ack(preq);
			break;
		default:
			log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK,	LOG_INFO, __func__, "resv_end event: accept req by default");
			reply_ack(preq);
	}
	return;
}

/**
 * @brief
 * 		req_deleteReservation - service the PBS_BATCH_DeleteResv Request
 *
 *		This request deletes a resources reservation if the requester
 *		is authorized to do this.
 *
 * @param[in]	preq	- Job Request
 *
 * @par	MT-safe: No
 */

void
req_deleteReservation(struct batch_request *preq)
{
	static int lenF = 6; /*strlen ("False") + 1*/

	resc_resv *presv;
	job *pjob;
	struct batch_request *newreq;
	struct work_task *pwt;

	char buf[PBS_MAXHOSTNAME + PBS_MAXUSER + 2]; /* temp, possibly remove in future */
	char user[PBS_MAXUSER + 1];
	char host[PBS_MAXHOSTNAME + 1];
	int perm;
	int relVal;
	int state, sub;
	long futuredr;

	/*Does resc_resv object exist and requester have enough priviledge?*/

	presv = chk_rescResv_request(preq->rq_ind.rq_delete.rq_objname, preq);

	/*Note: on failure, chk_rescResv_request invokes req_reject
	 *Appropriate reply got sent & batch_request got freed
	 */
	if (presv == NULL)
		return;

	/*Know resc_resv struct exists and requester allowed to remove it*/
	futuredr = presv->ri_futuredr;
	presv->ri_futuredr = 0; /*would be non-zero if getting*/
	/*here from task_list_timed*/
	(void) strcpy(user, preq->rq_user); /*need after request is gone*/
	(void) strcpy(host, preq->rq_host);
	perm = preq->rq_perm;

	/*Generate message(s) to reservation owner (listed users) as appropriate
	 *according to what was requested in the mailpoints attribute and who
	 *the submitter of the request happens to be (user, scheduler, or us)
	 */
	resv_mailAction(presv, preq);
	/*ck_submitClient_needs_reply()*/
	if (presv->ri_brp) {
		if (presv->ri_qs.ri_state == RESV_UNCONFIRMED) {
			if ((presv->ri_wattr[RESV_ATR_interactive].at_flags & ATR_VFLAG_SET) &&
				(presv->ri_wattr[RESV_ATR_interactive].at_val.at_long < 0) &&
				(futuredr != 0)) {

				sprintf(buf, "%s delete, wait period expired",
					presv->ri_qs.ri_resvID);
			} else {
				sprintf(buf, "%s DENIED", presv->ri_qs.ri_resvID);
			}

		} else {
			sprintf(buf, "%s BEING DELETED", presv->ri_qs.ri_resvID);
		}

		(void) reply_text(presv->ri_brp, PBSE_NONE, buf);
		presv->ri_brp = NULL;
	}


	(void) sprintf(buf, "%s@%s", preq->rq_user, preq->rq_host);
	(void) sprintf(log_buffer, "requestor=%s", buf);

	if (strcmp(presv->ri_wattr[RESV_ATR_resv_owner].at_val.at_str, buf))
		account_recordResv(PBS_ACCT_DRss, presv, log_buffer);
	else
		account_recordResv(PBS_ACCT_DRclient, presv, log_buffer);

	if (presv->ri_qs.ri_state != RESV_UNCONFIRMED) {
		char hook_msg[HOOK_MSG_SIZE] = {0};
		switch (process_hooks(preq, hook_msg, sizeof(hook_msg), pbs_python_set_interrupt)) {
	                case 0: /* explicit reject */
	                case 1: /* no recreate request as there are only read permissions */
	                case 2: /* no hook script executed - go ahead and accept event*/
	                        break;
	                default:
	                        log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK, LOG_INFO, __func__, "resv_end event: accept req by default");
        	}
	}


	/*If there are any jobs associated with the reservation, construct and
	 *issue a PBS_BATCH_DeleteJob request for each job.
	 *
	 *General notes on this process:
	 *Use issue_Drequest() to issue a PBS_BATCH_* request - can be to
	 *1) this server, 2) another server, 3) to a pbs_mom.
	 *
	 *In the present situation the server is going to issue the request to
	 *itself (a locally generated request).  The future "event" that's
	 *to occur, and which must be handled, is the reply fom this request.
	 *The handling task is initially placed on the server's "task_list_event"
	 *list as a task of type "WORK_Deferred_Local" and, a call is made to the
	 *general "dispatch" function (dispatch_request) to dispatch the request.
	 *In replying back to itself regarding the request to itself, function
	 *"reply_send" is called.  Since it is passed a pointer to the batch_request
	 *structure for the request that it received, it will note that the request
	 *came from itself (connection set to PBS_LOCAL_CONNECTION).  It finds the
	 *handling task on "task_list_event" by finding the task with task field
	 *"wt_parm1" set equal to the address of the batch request structure in
	 *question.  That task is moved off the "event_task_list" and put on the
	 *"immedite_task_list" where it can be found and invoked the next time that
	 *the server calls function "next_task" from it's main loop.
	 *The work_task function that's going to be invoked will be responding to
	 *the "reply" that comes back from the servers original request to itself.
	 *The handling function, in addition to whatever else it might do, does
	 *have the RESPONSIBILITY of calling free_br() to remove all memory associated
	 *with the batch_request structure.
	 */

	if (presv->ri_qs.ri_type == RESC_RESV_OBJECT
		&& presv->ri_qp != NULL
		&& presv->ri_qp->qu_numjobs > 0) {

		/*One or more jobs are attached to this resource reservation
		 *Issue a PBS_BATCH_Manager request to set "enable" to "False"
		 *for the queue (if it is not already so set), set "start" for
		 *the queue to "False" as well- so the scheduler will cease
		 *scheduling the jobs in queue, then issue a PBS_BATCH_DeleteJob
		 *request for each resident job.
		 */

		int deleteProblem = 0;
		job *pnxj;

		if (presv->ri_qp->qu_attr[QA_ATR_Enabled].at_val.at_long) {

			svrattrl *psatl;
			newreq = alloc_br(PBS_BATCH_Manager);
			if (newreq == NULL) {
				req_reject(PBSE_SYSTEM, 0, preq);
				return;
			}
			CLEAR_HEAD(newreq->rq_ind.rq_manager.rq_attr);

			newreq->rq_ind.rq_manager.rq_cmd = MGR_CMD_SET;
			newreq->rq_ind.rq_manager.rq_objtype = MGR_OBJ_QUEUE;
			(void) strcpy(newreq->rq_ind.rq_manager.rq_objname,
				presv->ri_qp->qu_qs.qu_name);
			(void) strcpy(newreq->rq_user, user);
			(void) strcpy(newreq->rq_host, host);
			newreq->rq_perm = perm;

			if ((psatl = attrlist_create(ATTR_enable, NULL, lenF))
				== NULL) {

				req_reject(PBSE_SYSTEM, 0, preq);
				free_br(newreq);
				return;
			}
			psatl->al_flags = que_attr_def[QA_ATR_Enabled].at_flags;
			strcpy(psatl->al_value, "False");
			append_link(&newreq->rq_ind.rq_manager.rq_attr, &psatl->al_link, psatl);

			if ((psatl = attrlist_create(ATTR_start, NULL, lenF))
				== NULL) {

				req_reject(PBSE_SYSTEM, 0, preq);
				free_br(newreq);
				return;
			}
			psatl->al_flags = que_attr_def[QA_ATR_Started].at_flags;
			strcpy(psatl->al_value, "False");
			append_link(&newreq->rq_ind.rq_manager.rq_attr,
				&psatl->al_link, psatl);

			if (issue_Drequest(PBS_LOCAL_CONNECTION, newreq,
				release_req, &pwt, 0) == -1) {
				req_reject(PBSE_SYSTEM, 0, preq);
				free_br(newreq);
				return;
			}
			/* set things so that any removal of the reservation
			 * structure also removes any "yet to be processed"
			 * work tasks that are associated with the reservation
			 */
			append_link(&presv->ri_svrtask, &pwt->wt_linkobj, pwt);

			tickle_for_reply();
		}

		/*Ok, input to the queue is stopped, try and delete jobs in queue*/

		relVal = 1;
		eval_resvState(presv, RESVSTATE_req_deleteReservation,
			relVal, &state, &sub);
		(void) resv_setResvState(presv, state, sub);
		pjob = (job *) GET_NEXT(presv->ri_qp->qu_jobs);
		while (pjob != NULL) {

			pnxj = (job *) GET_NEXT(pjob->ji_jobque);

			/*
			 * If a history job (job state is JOB_STATE_MOVED
			 * or JOB_STATE_FINISHED, then no need to delete
			 * it again as it is already deleted.
			 */
			if ((pjob->ji_qs.ji_state == JOB_STATE_MOVED) ||
				(pjob->ji_qs.ji_state == JOB_STATE_FINISHED)) {
				pjob = pnxj;
				continue;
			}
			newreq = alloc_br(PBS_BATCH_DeleteJob);
			if (newreq != NULL) {

				/*when owner of job is not same as owner of resv, */
				/*need extra permission; Also extra info for owner*/

				CLEAR_HEAD(newreq->rq_ind.rq_manager.rq_attr);
				newreq->rq_perm = perm | ATR_DFLAG_MGWR;
				newreq->rq_extend = NULL;

				/*reply processing needs resv*/
				newreq->rq_extra = (void *) presv;

				(void) strcpy(newreq->rq_user, user);
				(void) strcpy(newreq->rq_host, host);
				(void) strcpy(newreq->rq_ind.rq_delete.rq_objname,
					pjob->ji_qs.ji_jobid);

				if (issue_Drequest(PBS_LOCAL_CONNECTION, newreq,
					release_req, &pwt, 0) == -1) {
					deleteProblem++;
					free_br(newreq);
				}
				/* set things so that any removal of the reservation
				 * structure also removes any "yet to be processed"
				 * work tasks that are associated with the reservation
				 */
				append_link(&presv->ri_svrtask, &pwt->wt_linkobj, pwt);

				tickle_for_reply();
			} else
				deleteProblem++;

			pjob = pnxj;
		}

		if (deleteProblem) {
			/*some problems attempting to delete reservation's jobs
			 *shouldn't end up re-calling req_deleteReservation
			 */
			sprintf(log_buffer, "%s %s\n",
				"problem deleting jobs belonging to",
				presv->ri_qs.ri_resvID);
			(void) reply_text(preq, PBSE_RESVMSG, log_buffer);
		} else {
			/*no problems so far, we are attempting to do it
			 *If all job deletions succeed, resv_purge()
			 *should get triggered
			 */
			reply_ack(preq);

			/*
			 * If all the jobs in the RESV are history jobs, then
			 * better to purge the RESV now only without waiting
			 * for next resv delete iteration.
			 */
			pjob = NULL;
			if (presv && presv->ri_qp)
				pjob = (job *) GET_NEXT(presv->ri_qp->qu_jobs);
			while (pjob != NULL) {
				if ((pjob->ji_qs.ji_state != JOB_STATE_MOVED) &&
					(pjob->ji_qs.ji_state != JOB_STATE_FINISHED) &&
					(pjob->ji_qs.ji_state != JOB_STATE_EXPIRED))
					break;
				pjob = (job *) GET_NEXT(pjob->ji_jobque);
			}
			if (pjob == NULL) /* all are history jobs only */
				resv_purge(presv);
			else {
				/* other jobs remain, need to set task to monitor */
				/* when they are dequeued */
				pwt = set_task(WORK_Immed, 0, post_deljobfromresv_req,
					(void *) presv);
				if (pwt)
					append_link(&presv->ri_svrtask,
						&pwt->wt_linkobj, pwt);
			}
		}

		/*This is all we can do for now*/
		return;

	} else if (presv->ri_qs.ri_type == RESV_JOB_OBJECT ||
		presv->ri_qs.ri_type == RESC_RESV_OBJECT) {

		/*Ok, we have no jobs attached so can purge reservation
		 If reservation has an attached queue, a request to qmgr
		 will get made to delete the queue
		 */
		relVal = 2;
		eval_resvState(presv, RESVSTATE_req_deleteReservation,
			relVal, &state, &sub);
		(void) resv_setResvState(presv, state, sub);
		reply_ack(preq);
		resv_purge(presv);
		return;
	} else {
		/*Don't expect to ever see this message*/
		req_reject(PBSE_UNKRESVTYPE, 0, preq);
	}
}


/**
 * @brief
 * 		post_delete_route - complete the task of deleting a job which was
 *		being routed at the time the delete request was received.
 *
 *		Just recycle the delete request, the job will either be here or not.
 *
 * @param[in]	pwt	- work_task structure
 */

static void
post_delete_route(struct work_task *pwt)
{
	req_deletejob((struct batch_request *) pwt->wt_parm1);
	return;
}

/**
 * @brief
 * 		post_delete_mom1 - first of 2 work task trigger functions to finish the
 *		deleting of a running job.  This first part is invoked when MOM
 *		responds to the SIGTERM signal request.
 *
 * @param[in]	pwt	- work task
 */

static void
post_delete_mom1(struct work_task *pwt)
{
	int auxcode;
	job *pjob;
	struct batch_request *preq_sig; /* signal request to MOM */
	struct batch_request *preq_clt; /* original client request */
	int rc;
	int tries = 0;

	preq_sig = pwt->wt_parm1;
	rc = preq_sig->rq_reply.brp_code;
	auxcode = preq_sig->rq_reply.brp_auxcode;
	preq_clt = preq_sig->rq_extra;
	if (preq_clt == NULL) {
		release_req(pwt);
		return;
	}

	pjob = find_job(preq_sig->rq_ind.rq_signal.rq_jid);
	release_req(pwt);
	if (pjob == NULL) {
		/* job has gone away */
		req_reject(PBSE_UNKJOBID, 0, preq_clt);
		return;
	}

resend:
	if (rc) {
		/* mom rejected request */
		sprintf(log_buffer, "MOM rejected signal during delete (%d)", rc);
		log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
			pjob->ji_qs.ji_jobid, log_buffer);

		if (rc == PBSE_UNKSIG) {
			if (tries++) {
				req_reject(rc, 0, preq_clt);
				return;
			}
			/* 2nd try, use SIGTERM */
			rc = issue_signal(pjob, sigt, post_delete_mom1, preq_clt);
			if (rc == 0)
				return; /* will be back when replies */
			goto resend;
		} else if (rc == PBSE_UNKJOBID) {
			/* if job was in prerun state, cannot delete it, even if mom does not know
			 * about this job. Going ahead and deleting could result in a
			 * server crash, when post_sendmom completes.
			 */
			if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PRERUN) {
				req_reject(rc, 0, preq_clt);
				return;
			}

			/* MOM claims no knowledge, so just purge it */
			acct_del_write(pjob->ji_qs.ji_jobid, pjob, preq_clt, 0);
			/* removed the resources assigned to job */
			free_nodes(pjob);
			set_resc_assigned(pjob, 0, DECR);
			reply_ack(preq_clt);
			svr_saveorpurge_finjobhist(pjob);
		} else {
			req_reject(rc, 0, preq_clt);
		}
		return;
	}

	acct_del_write(pjob->ji_qs.ji_jobid, pjob, preq_clt, 0);
	reply_ack(preq_clt); /* dont need it, reply now */

	if (auxcode == JOB_SUBSTATE_TERM) {
		/* Mom running a site supplied Terminate Job script   */
		/* Put job into special Exiting state and we are done */

		(void) svr_setjobstate(pjob, JOB_STATE_EXITING, JOB_SUBSTATE_TERM);
		return;
	}
}

/**
 * @brief
 *		post_deljobfromresv_req
 *
 * @par Functionality:
 *
 *		This work_task function is triggered after all jobs in the queue
 *		associated with a reservation have had delete requests issued.
 *		If all jobs are  indeed found to be no longer present,
 *		the down counter in the reservation structure is
 *		decremented.  When the decremented value becomes less
 *		than or equal to zero, issue a request to delete the
 *		reservation.
 *
 *		If SERVER is configured for history jobs...
 *		If the reservation down counter is positive, check if all
 *		the jobs in the resv are history jobs. If yes, purge the
 *		reservation using resv_purge() without waiting.
 *
 *		If there are still non-history jobs, recall itself after 30 seconds.
 *
 * @param[in]	pwt	- pointer to the work task, the reservation structure
 *			  			pointer is contained in wt_parm1
 *
 * @return	void
 *
 */

static void
post_deljobfromresv_req(pwt)
struct work_task *pwt;
{
	resc_resv *presv;
	job *pjob = NULL;

	presv = (resc_resv *)((struct batch_request *) pwt->wt_parm1);

	/* return if presv is not valid */
	if (presv == NULL)
		return;

	if (presv->ri_qs.ri_type == RESC_RESV_OBJECT) {
		presv->ri_downcnt = presv->ri_qp->qu_numjobs;
		if (presv->ri_downcnt != 0) {
			if (presv->ri_qp)
				pjob = (job *) GET_NEXT(presv->ri_qp->qu_jobs);
			while (pjob != NULL) {
				if ((pjob->ji_qs.ji_state != JOB_STATE_MOVED) &&
					(pjob->ji_qs.ji_state != JOB_STATE_FINISHED) &&
					(pjob->ji_qs.ji_state != JOB_STATE_EXPIRED))
					break;
				pjob = (job *) GET_NEXT(pjob->ji_jobque);
			}
			/*
			 * If pjob is NULL, then all are history jobs only,
			 * make the ri_downcnt to 0, so that resv_purge()
			 * can be called down.
			 */
			if (pjob == NULL)
				presv->ri_downcnt = 0;
		}
	} else {
		return; /* not a reservation object, do nothing */
	}

	if (presv->ri_downcnt == 0) {
		resv_purge(presv);
	} else if (pjob) {
		/* one or more jobs still not able to be deleted; set me up for
		 * another call for 30 seconds into the future.
		 */
		pwt = set_task(WORK_Timed, time_now + 30, post_deljobfromresv_req,
			(void *) presv);
		if (pwt)
			append_link(&presv->ri_svrtask, &pwt->wt_linkobj, pwt);
	}
}
