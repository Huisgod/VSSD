// DiskSim SSD support
// ©2008 Microsoft Corporation. All Rights Reserved

#include "ssd.h"
#include "ssd_timing.h"
#include "ssd_clean.h"
#include "ssd_gang.h"
#include "ssd_init.h"
#include "modules/ssdmodel_ssd_param.h"

#ifndef sprintf_s
#define sprintf_s3(x,y,z) sprintf(x,z)
#define sprintf_s4(x,y,z,w) sprintf(x,z,w)
#define sprintf_s5(x,y,z,w,s) sprintf(x,z,w,s)
#else
#define sprintf_s3(x,y,z) sprintf_s(x,z)
#define sprintf_s4(x,y,z,w) sprintf_s(x,z,w)
#define sprintf_s5(x,y,z,w,s) sprintf_s(x,z,w,s)
#endif

#ifndef _strdup
#define _strdup strdup
#endif

static void ssd_request_complete(ioreq_event *curr);
static void ssd_media_access_request(ioreq_event *curr);

static void vssd_initialize(ioreq_event *curr);
static void vssd_deallocating(ioreq_event *curr);
static void vssd_push_event_to_user_queue(ioreq_event *curr);
static void vssd_choose_user_to_run(ioreq_event *curr);
static void vssd_giving_per_user_quota(ioreq_event *curr);

static void vssd_finish_request(ioreq_event *curr);

struct ssd *getssd (int devno)
{
   struct ssd *s;
   ASSERT1((devno >= 0) && (devno < MAXDEVICES), "devno", devno);

   s = disksim->ssdinfo->ssds[devno];
   return (disksim->ssdinfo->ssds[devno]);
}

int ssd_set_depth (int devno, int inbusno, int depth, int slotno)
{
   ssd_t *currdisk;
   int cnt;

   currdisk = getssd (devno);
   assert(currdisk);
   cnt = currdisk->numinbuses;
   currdisk->numinbuses++;
   if ((cnt + 1) > MAXINBUSES) {
      fprintf(stderr, "Too many inbuses specified for ssd %d - %d\n", devno, (cnt+1));
      exit(1);
   }
   currdisk->inbuses[cnt] = inbusno;
   currdisk->depth[cnt] = depth;
   currdisk->slotno[cnt] = slotno;
   return(0);
}

int ssd_get_depth (int devno)
{
   ssd_t *currdisk;
   currdisk = getssd (devno);
   return(currdisk->depth[0]);
}

int ssd_get_slotno (int devno)
{
   ssd_t *currdisk;
   currdisk = getssd (devno);
   return(currdisk->slotno[0]);
}

int ssd_get_inbus (int devno)
{
   ssd_t *currdisk;
   currdisk = getssd (devno);
   return(currdisk->inbuses[0]);
}

int ssd_get_maxoutstanding (int devno)
{
   ssd_t *currdisk;
   currdisk = getssd (devno);
   return(currdisk->maxqlen);
}

double ssd_get_blktranstime (ioreq_event *curr)
{
   ssd_t *currdisk;
   double tmptime;

   currdisk = getssd (curr->devno);
   tmptime = bus_get_transfer_time(ssd_get_busno(curr), 1, (curr->flags & READ));
   if (tmptime < currdisk->blktranstime) {
      tmptime = currdisk->blktranstime;
   }
   return(tmptime);
}

int ssd_get_busno (ioreq_event *curr)
{
   ssd_t *currdisk;
   intchar busno;
   int depth;

   currdisk = getssd (curr->devno);
   busno.value = curr->busno;
   depth = currdisk->depth[0];
   return(busno.byte[depth]);
}

static void ssd_assert_current_activity(ssd_t *currdisk, ioreq_event *curr)
{
    assert(currdisk->channel_activity != NULL &&
        currdisk->channel_activity->devno == curr->devno &&
        currdisk->channel_activity->blkno == curr->blkno &&
        currdisk->channel_activity->bcount == curr->bcount);
}

/*
 * ssd_send_event_up_path()
 *
 * Acquires the bus (if not already acquired), then uses bus_delay to
 * send the event up the path.
 *
 * If the bus is already owned by this device or can be acquired
 * immediately (interleaved bus), the event is sent immediately.
 * Otherwise, ssd_bus_ownership_grant will later send the event.
 */
static void ssd_send_event_up_path (ioreq_event *curr, double delay)
{
   ssd_t *currdisk;
   int busno;
   int slotno;

   // fprintf (outputfile, "ssd_send_event_up_path - devno %d, type %d, cause %d, blkno %d\n", curr->devno, curr->type, curr->cause, curr->blkno);

   currdisk = getssd (curr->devno);

   ssd_assert_current_activity(currdisk, curr);

   busno = ssd_get_busno(curr);
   slotno = currdisk->slotno[0];

   /* Put new request at head of buswait queue */
   curr->next = currdisk->buswait;
   currdisk->buswait = curr;

   curr->tempint1 = busno;
   curr->time = delay;
   if (currdisk->busowned == -1) {

      // fprintf (outputfile, "Must get ownership of the bus first\n");

      if (curr->next) {
         //fprintf(stderr,"Multiple bus requestors detected in ssd_send_event_up_path\n");
         /* This should be ok -- counting on the bus module to sequence 'em */
      }
      if (bus_ownership_get(busno, slotno, curr) == FALSE) {
         /* Remember when we started waiting (only place this is written) */
         currdisk->stat.requestedbus = simtime;
      } else {
         currdisk->busowned = busno;
         bus_delay(busno, DEVICE, curr->devno, delay, curr); /* Never for SCSI */
      }
   } else if (currdisk->busowned == busno) {

      //fprintf (outputfile, "Already own bus - so send it on up\n");

      bus_delay(busno, DEVICE, curr->devno, delay, curr);
   } else {
      fprintf(stderr, "Wrong bus owned for transfer desired\n");
      exit(1);
   }
}

/* The idea here is that only one request can "possess" the channel back to the
   controller at a time. All others are enqueued on queue of pending activities.
   "Completions" ... those operations that need only be signaled as done to the
   controller ... are given on this queue.  The "channel_activity" field indicates
   whether any operation currently possesses the channel.

   It is our hope that new requests cannot enter the system when the channel is
   possessed by another operation.  This would not model reality!!  However, this
   code (and that in ssd_request_arrive) will handle this case "properly" by enqueuing
   the incoming request.  */

static void ssd_check_channel_activity (ssd_t *currdisk)
{
   while (1) {
       ioreq_event *curr = currdisk->completion_queue;
       currdisk->channel_activity = curr;
       if (curr != NULL) {
           currdisk->completion_queue = curr->next;
           if (currdisk->neverdisconnect) {
               /* already connected */
               if (curr->flags & READ) {
                   /* transfer data up the line: curr->bcount, which is still set to */
                   /* original requested value, indicates how many blks to transfer. */
                   curr->type = DEVICE_DATA_TRANSFER_COMPLETE;
                   ssd_send_event_up_path(curr, (double) 0.0);
               } else {
                   	ssd_request_complete (curr);
               }
           } else {
               /* reconnect to controller */
               curr->type = IO_INTERRUPT_ARRIVE;
               curr->cause = RECONNECT;
               ssd_send_event_up_path (curr, currdisk->bus_transaction_latency);
               currdisk->reconnect_reason = DEVICE_ACCESS_COMPLETE;
           }
       } else {
           curr = ioqueue_get_next_request(currdisk->queue);
           currdisk->channel_activity = curr;
           if (curr != NULL) {
               if (curr->flags & READ) {
                   if (!currdisk->neverdisconnect) {
                       ssd_media_access_request(ioreq_copy(curr));
                       curr->type = IO_INTERRUPT_ARRIVE;
                       curr->cause = DISCONNECT;
                       ssd_send_event_up_path (curr, currdisk->bus_transaction_latency);
                   } else {
                       ssd_media_access_request(curr);
                       continue;
                   }
               } else {
                   curr->cause = RECONNECT;
                   curr->type = IO_INTERRUPT_ARRIVE;
                   currdisk->reconnect_reason = IO_INTERRUPT_ARRIVE;
                   ssd_send_event_up_path (curr, currdisk->bus_transaction_latency);
               }
           }
       }
       break;
   }
}

/*
 * ssd_bus_ownership_grant
 * Calls bus_delay to handle the event that the disk has been granted the bus.  I believe
 * this is always initiated by a call to ssd_send_even_up_path.
 */
void ssd_bus_ownership_grant (int devno, ioreq_event *curr, int busno, double arbdelay)
{
   ssd_t *currdisk;
   ioreq_event *tmp;

   currdisk = getssd (devno);

   ssd_assert_current_activity(currdisk, curr);
   tmp = currdisk->buswait;
   while ((tmp != NULL) && (tmp != curr)) {
      tmp = tmp->next;
   }
   if (tmp == NULL) {
      fprintf(stderr, "Bus ownership granted to unknown ssd request - devno %d, busno %d\n", devno, busno);
      exit(1);
   }
   currdisk->busowned = busno;
   currdisk->stat.waitingforbus += arbdelay;
   //ASSERT (arbdelay == (simtime - currdisk->stat.requestedbus));
   currdisk->stat.numbuswaits++;
   bus_delay(busno, DEVICE, devno, tmp->time, tmp);
}

void ssd_bus_delay_complete (int devno, ioreq_event *curr, int sentbusno)
{
   ssd_t *currdisk;
   intchar slotno;
   intchar busno;
   int depth;

   currdisk = getssd (devno);
   ssd_assert_current_activity(currdisk, curr);

   // fprintf (outputfile, "Entered ssd_bus_delay_complete\n");

   // EPW: I think the buswait logic doesn't do anything, is confusing, and risks
   // overusing the "next" field, although an item shouldn't currently be a queue.
   if (curr == currdisk->buswait) {
      currdisk->buswait = curr->next;
   } else {
      ioreq_event *tmp = currdisk->buswait;
      while ((tmp->next != NULL) && (tmp->next != curr)) {
         tmp = tmp->next;
      }
      if (tmp->next != curr) {
          // fixed a warning here
          //fprintf(stderr, "Bus delay complete for unknown ssd request - devno %d, busno %d\n", devno, busno.value);
          fprintf(stderr, "Bus delay complete for unknown ssd request - devno %d, busno %d\n", devno, curr->busno);
         exit(1);
      }
      tmp->next = curr->next;
   }
   busno.value = curr->busno;
   slotno.value = curr->slotno;
   depth = currdisk->depth[0];
   slotno.byte[depth] = slotno.byte[depth] >> 4;
   curr->time = 0.0;
   if (depth == 0) {
      intr_request ((event *)curr);
   } else {
      bus_deliver_event(busno.byte[depth], slotno.byte[depth], curr);
   }
}


/*
 * send completion up the line
 */
static void ssd_request_complete(ioreq_event *curr)	//*jian vsd's request schedule (2) 
{							//*jian vsd schedule (1) in src/disksim.c
   ssd_t *currdisk;
   ioreq_event *x;

   //fprintf (stderr, "Entering ssd_request_complete: %12.6f\n", simtime);
   ASSERT(curr->user_id>0);

   currdisk = getssd (curr->devno);
   ssd_assert_current_activity(currdisk, curr);

   if ((x = ioqueue_physical_access_done(currdisk->queue,curr)) == NULL) {
      fprintf(stderr, "ssd_request_complete:  ioreq_event not found by ioqueue_physical_access_done call\n");
      exit(1);
   }
   else
   vssd_finish_request(curr);

   /* send completion interrupt */
   curr->type = IO_INTERRUPT_ARRIVE;
   curr->cause = COMPLETION;
   ssd_send_event_up_path(curr, currdisk->bus_transaction_latency);
}

static void ssd_bustransfer_complete (ioreq_event *curr)
{
   // fprintf (outputfile, "Entering ssd_bustransfer_complete for disk %d: %12.6f\n", curr->devno, simtime);

   if (curr->flags & READ) {
      ssd_request_complete (curr);
   } else {
      ssd_t *currdisk = getssd (curr->devno);
      ssd_assert_current_activity(currdisk, curr);
      if (currdisk->neverdisconnect == FALSE) {
          /* disconnect from bus */
          ioreq_event *tmp = ioreq_copy (curr);
          tmp->type = IO_INTERRUPT_ARRIVE;
          tmp->cause = DISCONNECT;
          ssd_send_event_up_path (tmp, currdisk->bus_transaction_latency);
          ssd_media_access_request (curr);
      } else {
          ssd_media_access_request (curr);
          ssd_check_channel_activity (currdisk);
      }
   }
}

/*
 * returns the logical page number within an element given a block number as
 * issued by the file system
 */
int ssd_logical_pageno(int blkno, ssd_t *s, int user_id)
{
    int apn;
    int lpn;

	/*#ifdef ALLOCATING_ELEMENT
	user_time_state *ptr;
        ptr = find_user(user_id);
        //blkno = blkno - ( ptr->first_log_blk_address * s->params.pages_per_block * s->params.page_size);
        blkno = blkno - ( ptr->first_log_blk_address * SSD_DATA_PAGES_PER_BLOCK(s) * s->params.page_size);
	apn = blkno/s->params.page_size;
	lpn = ((apn - (apn % (s->params.element_stride_pages * ptr->element_allcating_count)))/
                      ptr->element_allcating_count) + (apn % s->params.element_stride_pages);

	return lpn;
	#endif
	#ifdef ALLOCATING_PLANE
	allocating_infor *ptr;
	user_time_state *ptr_2;
	ptr_2=find_user(user_id);
	//blkno = blkno - ( ptr_2->first_log_blk_address * s->params.pages_per_block * s->params.page_size);
        blkno = blkno - ( ptr_2->first_log_blk_address * SSD_DATA_PAGES_PER_BLOCK(s) * s->params.page_size);
	apn = blkno/s->params.page_size;

	int element_num;
	element_num =\
	(blkno/(s->params.element_stride_pages*s->params.page_size)) % allocating_infor_head[user_id]->plane_allcating_count % s->params.nelements;
	ptr = find_element(allocating_infor_head[user_id],element_num);


	lpn = (apn / allocating_infor_head[user_id]->plane_allcating_count) * ptr->plane_allcating_count;
	lpn += (apn % allocating_infor_head[user_id]->plane_allcating_count) / s->params.nelements;
	lpn += ptr->start_page_no;
    	return lpn;
	#endif*/
    // absolute page number is the block number as written by the above layer
    apn = blkno/s->params.page_size;

    // find the logical page number within the ssd element. we maintain the
    // mapping between the logical page number and the actual physical page
    // number. an alternative is that we could maintain the mapping between
    // apn we calculated above and the physical page number. but the range
    // of apn is several times bigger and so we chose to go with the mapping
    // b/w lpn --> physical page number
    //lpn = ((apn - (apn % (s->params.element_stride_pages * s->params.nelements)))/
    //                  s->params.nelements) + (apn % s->params.element_stride_pages);
    //lpn = apn / s->params.nelements;

    //return lpn;
    return apn;
}

int ssd_already_present(ssd_req **reqs, int total, ioreq_event *req)
{
    int i;
    int found = 0;

    for (i = 0; i < total; i ++) {
        if ((req->blkno == reqs[i]->org_req->blkno) &&
            (req->flags == reqs[i]->org_req->flags)) {
            found = 1;
            break;
        }
    }

    return found;
}

double _ssd_invoke_element_cleaning(int elem_num, ssd_t *s)
{
    double clean_cost = ssd_clean_element(s, elem_num);
    return clean_cost;
}

static int ssd_invoke_element_cleaning(int elem_num, ssd_t *s)
{
    double max_cost = 0;
    int cleaning_invoked = 0;
    ssd_element *elem = &s->elements[elem_num];
    ssd_element_metadata *metadata = &s->elements[elem_num].metadata;//*jian add


    // element must be free
    ASSERT(elem->media_busy == FALSE);
    ASSERT(metadata->user_id > 0);

    max_cost = _ssd_invoke_element_cleaning(elem_num, s);

	//*====================jian add=========================
	user_time_state *ptr;
	ptr = find_user(metadata->user_id_2);
	#ifdef VSSD_QUANTA_BASED_SCHEDULER
		ptr->curr_quota -= max_cost;
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY
		metadata->simtime_for_quanta_base_schedule[1] += max_cost;
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY_SEPARATE_USER
		metadata->simtime_for_quanta_base_schedule[metadata->user_id_2] += max_cost;
	#endif
	ptr->total_clean_time_per_sec += max_cost;
	//*====================jian add=========================

    // cleaning was invoked on this element. we can start
    // the next operation on this elem only after the cleaning
    // gets over.
    if (max_cost > 0) {
        ioreq_event *tmp;

        elem->media_busy = 1;
        cleaning_invoked = 1;

        // we use the 'blkno' field to store the element number
        tmp = (ioreq_event *)getfromextraq();
        tmp->devno = s->devno;
        tmp->time = simtime + max_cost;
        tmp->blkno = elem_num;
        tmp->ssd_elem_num = elem_num;
        tmp->type = SSD_CLEAN_ELEMENT;
        tmp->flags = SSD_CLEAN_ELEMENT;
        tmp->busno = -1;
        tmp->bcount = -1;
        stat_update (&s->stat.acctimestats, max_cost);
        addtointq ((event *)tmp);

        // stat
        elem->stat.tot_clean_time += max_cost;
	
    }

    return cleaning_invoked;
}

static void ssd_activate_elem(ssd_t *currdisk, int elem_num)
{
    ioreq_event *req;
    ssd_req **read_reqs;
    ssd_req **write_reqs;
    int i;
    int read_total = 0;
    int write_total = 0;
    double schtime = 0;
    int max_reqs;
    int tot_reqs_issued;
    double max_time_taken = 0;


    ssd_element *elem = &currdisk->elements[elem_num];

    // if the media is busy, we can't do anything, so return
    if (elem->media_busy == TRUE) {
        return;
    }
	//*jian add
	req = ioqueue_show_next_request(elem->queue);
	if(req != NULL)
	{
		elem->metadata.user_id_2 = req->user_id;
		#ifdef CURREN_BLOCK_ENABLE
		elem->metadata.user_id = req->user_id;
		#endif
		#ifndef CURREN_BLOCK_ENABLE
		elem->metadata.user_id = 1;
		#endif
	}
	//*jian


    ASSERT(ioqueue_get_reqoutstanding(elem->queue) == 0);

    // we can invoke cleaning in the background whether there
    // is request waiting or not
    if (currdisk->params.cleaning_in_background) {
        // if cleaning was invoked, wait until
        // it is over ...
	if (elem->metadata.reqs_waiting > 0)
	{
        	if (ssd_invoke_element_cleaning(elem_num, currdisk)) {
            		return;
        	}
	}
	else	//*jian background GC
	{
        	if (ssd_invoke_element_cleaning(elem_num, currdisk)) {
            		return;
        	}
	}
    }

    ASSERT(elem->metadata.reqs_waiting == ioqueue_get_number_in_queue(elem->queue));

    if (elem->metadata.reqs_waiting > 0) {

        // invoke cleaning in foreground when there are requests waiting
        if (!currdisk->params.cleaning_in_background) {
            // if cleaning was invoked, wait until
            // it is over ...
            if (ssd_invoke_element_cleaning(elem_num, currdisk)) {
                return;
            }
        }

        // how many reqs can we issue at once
        if (currdisk->params.copy_back == SSD_COPY_BACK_DISABLE) {
            max_reqs = 1;
        } else {
            if (currdisk->params.num_parunits == 1) {
                max_reqs = 1;
            } else {
                max_reqs = MAX_REQS_ELEM_QUEUE;
            }
        }

        // ideally, we should issue one req per plane, overlapping them all.
        // in order to simplify the overlapping strategy, let's issue
        // requests of the same type together.

        read_reqs = (ssd_req **) malloc(max_reqs * sizeof(ssd_req *));
        write_reqs = (ssd_req **) malloc(max_reqs * sizeof(ssd_req *));

        // collect the requests
        while ((req = ioqueue_get_next_request(elem->queue)) != NULL) {
            int found = 0;

            elem->metadata.reqs_waiting --;

            // see if we already have the same request in the list.
            // this usually doesn't happen -- but on synthetic traces
            // this weird case can occur.
            if (req->flags & READ) {
                found = ssd_already_present(read_reqs, read_total, req);
            } else {
                found = ssd_already_present(write_reqs, write_total, req);
            }

            if (!found) {
                // this is a valid request
                ssd_req *r = malloc(sizeof(ssd_req));
                r->blk = req->blkno;
                r->count = req->bcount;
                r->is_read = req->flags & READ;
                r->org_req = req;
                r->plane_num = -1; // we don't know to which plane this req will be directed at
		#ifdef PAGE_LEVEL_MAPPING
                r->plane_num = req->w_plane; // we don't know to which plane this req will be directed at
		#endif
		r->user_id = req->user_id;

                if (req->flags & READ) {
                    read_reqs[read_total] = r;
                    read_total ++;
                } else {
                    write_reqs[write_total] = r;
                    write_total ++;
                }

                // if we have more reqs than we can handle, quit
                if ((read_total >= max_reqs) ||
                    (write_total >= max_reqs)) {
                    break;
                }
            } else {
                // throw this request -- it doesn't make sense
                stat_update (&currdisk->stat.acctimestats, 0);
                req->time = simtime;
                req->ssd_elem_num = elem_num;
                req->type = DEVICE_ACCESS_COMPLETE;
                addtointq ((event *)req);
            }
        }

        if (read_total > 0) {
            // first issue all the read requests (it doesn't matter what we
            // issue first). i chose read because reads are mostly synchronous.
            // find the time taken to serve these requests.
            ssd_compute_access_time(currdisk, elem_num, read_reqs, read_total);

            // add an event for each request completion
            for (i = 0; i < read_total; i ++) {
              elem->media_busy = TRUE;
	//*====================jian add=========================
	#ifdef VSSD_QUANTA_BASED_SCHEDULER
	    	user_time_state *ptr;
		ptr = find_user(read_reqs[i]->org_req->user_id);
		ptr->curr_quota -= read_reqs[i]->schtime;
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY
	    	user_time_state *ptr;
		ptr = find_user(read_reqs[i]->org_req->user_id);
		elem->metadata.simtime_for_quanta_base_schedule[1] += read_reqs[i]->schtime;
		ptr->curr_quota -= ((elem->metadata.simtime_for_quanta_base_schedule[1]) - read_reqs[i]->org_req->start_service_time);
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY_SEPARATE_USER
	    	user_time_state *ptr;
		ptr = find_user(read_reqs[i]->org_req->user_id);
		elem->metadata.simtime_for_quanta_base_schedule[read_reqs[i]->org_req->user_id] += read_reqs[i]->schtime;
		ptr->curr_quota -= ((elem->metadata.simtime_for_quanta_base_schedule[read_reqs[i]->org_req->user_id]) - read_reqs[i]->org_req->start_service_time);
	#endif
	//*====================jian add=========================

              // find the maximum time taken by a request
              if (schtime < read_reqs[i]->schtime) {
                  schtime = read_reqs[i]->schtime;
              }

              stat_update (&currdisk->stat.acctimestats, read_reqs[i]->acctime);
              read_reqs[i]->org_req->time = simtime + read_reqs[i]->schtime;
              read_reqs[i]->org_req->ssd_elem_num = elem_num;
              read_reqs[i]->org_req->type = DEVICE_ACCESS_COMPLETE;

              //printf("R: blk %d elem %d acctime %f simtime %f\n", read_reqs[i]->blk,
                //  elem_num, read_reqs[i]->acctime, read_reqs[i]->org_req->time);

              addtointq ((event *)read_reqs[i]->org_req);
              free(read_reqs[i]);
            }
        }

        free(read_reqs);

        max_time_taken = schtime;

        if (write_total > 0) {
            // next issue the write requests
            ssd_compute_access_time(currdisk, elem_num, write_reqs, write_total);

            // add an event for each request completion.
            // note that we can issue the writes only after all the reads above are
            // over. so, include the maximum read time when creating the event.
            for (i = 0; i < write_total; i ++) {
              elem->media_busy = TRUE;

	//*====================jian add=========================
	#ifdef VSSD_QUANTA_BASED_SCHEDULER
	    	user_time_state *ptr;
		ptr = find_user(write_reqs[i]->org_req->user_id);
		ptr->curr_quota -= write_reqs[i]->schtime;
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY
	    	user_time_state *ptr;
		ptr = find_user(write_reqs[i]->org_req->user_id);
		elem->metadata.simtime_for_quanta_base_schedule[1] += write_reqs[i]->schtime;
		ptr->curr_quota -=((elem->metadata.simtime_for_quanta_base_schedule[1]) - write_reqs[i]->org_req->start_service_time);
	#endif
	#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY_SEPARATE_USER
	    	user_time_state *ptr;
		ptr = find_user(write_reqs[i]->org_req->user_id);
		elem->metadata.simtime_for_quanta_base_schedule[write_reqs[i]->org_req->user_id] += write_reqs[i]->schtime;
		ptr->curr_quota -=((elem->metadata.simtime_for_quanta_base_schedule[write_reqs[i]->org_req->user_id]) - write_reqs[i]->org_req->start_service_time);
	#endif
	//*====================jian add=========================

              stat_update (&currdisk->stat.acctimestats, write_reqs[i]->acctime);
              write_reqs[i]->org_req->time = simtime + schtime + write_reqs[i]->schtime;
              //printf("blk %d elem %d acc time %f\n", write_reqs[i]->blk, elem_num, write_reqs[i]->acctime);
              if (max_time_taken < (schtime+write_reqs[i]->schtime)) {
                  max_time_taken = (schtime+write_reqs[i]->schtime);
              }

              write_reqs[i]->org_req->ssd_elem_num = elem_num;
              write_reqs[i]->org_req->type = DEVICE_ACCESS_COMPLETE;
              //printf("W: blk %d elem %d acctime %f simtime %f\n", write_reqs[i]->blk,
                //  elem_num, write_reqs[i]->acctime, write_reqs[i]->org_req->time);

              addtointq ((event *)write_reqs[i]->org_req);
              free(write_reqs[i]);
            }
        }

        free(write_reqs);

        // statistics
        tot_reqs_issued = read_total + write_total;
        ASSERT(tot_reqs_issued > 0);
        currdisk->elements[elem_num].stat.tot_reqs_issued += tot_reqs_issued;
        currdisk->elements[elem_num].stat.tot_time_taken += max_time_taken;
    }
}


static void ssd_media_access_request_element (ioreq_event *curr)
{
   ssd_t *currdisk = getssd(curr->devno);
   int blkno = curr->blkno;
   int count = curr->bcount;

   int user_id = curr->user_id;


   /* **** CAREFUL ... HIJACKING tempint2 and tempptr2 fields here **** */
   curr->tempint2 = count;
   while (count != 0) {

       // find the element (package) to direct the request
       // create a new sub-request for the element
       ioreq_event *tmp = (ioreq_event *)getfromextraq();

       int elem_num = currdisk->timing_t->choose_element(currdisk->timing_t, blkno,curr->user_id,curr->flags,tmp);
       ssd_element *elem = &currdisk->elements[elem_num];

       tmp->user_id = curr->user_id;//new variable user_id (put into child request)
       tmp->devno = curr->devno;
       tmp->busno = curr->busno;
       tmp->flags = curr->flags;
       tmp->blkno = blkno;
       tmp->bcount = ssd_choose_aligned_count(currdisk->params.page_size, blkno, count);//*jian stripping a page to the element
#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY
       tmp->start_service_time = elem->metadata.simtime_for_quanta_base_schedule[1];
#endif
#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY_SEPARATE_USER
       tmp->start_service_time = elem->metadata.simtime_for_quanta_base_schedule[tmp->user_id];
#endif

       ASSERT(tmp->bcount == currdisk->params.page_size);

       tmp->tempptr2 = curr;
       blkno += tmp->bcount;
       count -= tmp->bcount;

       elem->metadata.reqs_waiting ++;

       // add the request to the corresponding element's queue
       ioqueue_add_new_request(elem->queue, (ioreq_event *)tmp);
       ssd_activate_elem(currdisk, elem_num);
   }
}

static void ssd_media_access_request (ioreq_event *curr)
{
    ssd_t *currdisk = getssd(curr->devno);
    switch(currdisk->params.alloc_pool_logic) {
        case SSD_ALLOC_POOL_PLANE:
        case SSD_ALLOC_POOL_CHIP:
            ssd_media_access_request_element(curr);
        break;

        case SSD_ALLOC_POOL_GANG:
#if SYNC_GANG
            ssd_media_access_request_gang_sync(curr);
#else
            ssd_media_access_request_gang(curr);
#endif
        break;

        default:
            printf("Unknown alloc pool logic %d\n", currdisk->params.alloc_pool_logic);
            ASSERT(0);
    }
}

static void ssd_reconnect_done (ioreq_event *curr)
{
   ssd_t *currdisk;

   // fprintf (outputfile, "Entering ssd_reconnect_done for disk %d: %12.6f\n", curr->devno, simtime);

   currdisk = getssd (curr->devno);
   ssd_assert_current_activity(currdisk, curr);

   if (curr->flags & READ) {
      if (currdisk->neverdisconnect) {
         /* Just holding on to bus; data transfer will be initiated when */
         /* media access is complete.                                    */
         addtoextraq((event *) curr);
         ssd_check_channel_activity (currdisk);
      } else {
         /* data transfer: curr->bcount, which is still set to original */
         /* requested value, indicates how many blks to transfer.       */
         curr->type = DEVICE_DATA_TRANSFER_COMPLETE;
         ssd_send_event_up_path(curr, (double) 0.0);
      }

   } else {
      if (currdisk->reconnect_reason == DEVICE_ACCESS_COMPLETE) {
         ssd_request_complete (curr);

      } else {
         /* data transfer: curr->bcount, which is still set to original */
         /* requested value, indicates how many blks to transfer.       */
         curr->type = DEVICE_DATA_TRANSFER_COMPLETE;
         ssd_send_event_up_path(curr, (double) 0.0);
      }
   }
}

static void ssd_request_arrive (ioreq_event *curr)
{
   ssd_t *currdisk;

   // fprintf (outputfile, "Entering ssd_request_arrive: %12.6f\n", simtime);
   // fprintf (outputfile, "ssd = %d, blkno = %d, bcount = %d, read = %d\n",curr->devno, curr->blkno, curr->bcount, (READ & curr->flags));

   currdisk = getssd(curr->devno);

   /* verify that request is valid. */
   if ((curr->blkno < 0) || (curr->bcount <= 0) ||
       ((curr->blkno + curr->bcount) > currdisk->numblocks)) {
      fprintf(stderr, "Invalid set of blocks requested from ssd - blkno %d, bcount %d, numblocks %d\n", curr->blkno, curr->bcount, currdisk->numblocks);
      exit(1);
   }

   /* create a new request, set it up for initial interrupt */
   ioqueue_add_new_request(currdisk->queue, curr);
   if (currdisk->channel_activity == NULL) {

      curr = ioqueue_get_next_request(currdisk->queue);
      currdisk->busowned = ssd_get_busno(curr);
      currdisk->channel_activity = curr;
      currdisk->reconnect_reason = IO_INTERRUPT_ARRIVE;

      if (curr->flags & READ) {
          if (!currdisk->neverdisconnect) {
              ssd_media_access_request (ioreq_copy(curr));
              curr->cause = DISCONNECT;
              curr->type = IO_INTERRUPT_ARRIVE;
              ssd_send_event_up_path(curr, currdisk->bus_transaction_latency);
          } else {
              ssd_media_access_request (curr);
              ssd_check_channel_activity(currdisk);
          }
      } else {
         curr->cause = READY_TO_TRANSFER;
         curr->type = IO_INTERRUPT_ARRIVE;
         ssd_send_event_up_path(curr, currdisk->bus_transaction_latency);
      }
   }
}

/*
 * cleaning in an element is over.
 */
static void ssd_clean_element_complete(ioreq_event *curr)
{
   ssd_t *currdisk;
   int elem_num;

   currdisk = getssd (curr->devno);
   elem_num = curr->ssd_elem_num;
   ASSERT(currdisk->elements[elem_num].media_busy == TRUE);

   // release this event
   addtoextraq((event *) curr);

   // activate the gang to serve the next set of requests
   currdisk->elements[elem_num].media_busy = 0;
   ssd_activate_elem(currdisk, elem_num);
}

void ssd_complete_parent(ioreq_event *curr, ssd_t *currdisk)
{
    ioreq_event *parent;

    /* **** CAREFUL ... HIJACKING tempint2 and tempptr2 fields here **** */
    parent = curr->tempptr2;
    parent->tempint2 -= curr->bcount;

    if (parent->tempint2 == 0) {
      ioreq_event *prev;

      assert(parent != currdisk->channel_activity);
      prev = currdisk->completion_queue;
      if (prev == NULL) {
         currdisk->completion_queue = parent;
         parent->next = prev;
      } else {
         while (prev->next != NULL)
            prev = prev->next;
            parent->next = prev->next;
            prev->next = parent;
      }
      if (currdisk->channel_activity == NULL) {
         ssd_check_channel_activity (currdisk);
      }
    }
}

static void ssd_access_complete_element(ioreq_event *curr)
{
   ssd_t *currdisk;
   int elem_num;
   ssd_element  *elem;
   ioreq_event *x;

   currdisk = getssd (curr->devno);
   //elem_num = currdisk->timing_t->choose_element(currdisk->timing_t, curr->blkno,curr->user_id,READ,curr);
   elem_num = curr->ssd_elem_num;

   /*if(elem_num != curr->ssd_elem_num)
   {
	fprintf(stderr,"&curr = %d\n",curr);
	fprintf(stderr,"curr->type = %d\n",curr->type);
	fprintf(stderr,"curr->blkno = %d\n",curr->blkno);
	fprintf(stderr,"curr->bcount = %d\n",curr->bcount);
	fprintf(stderr,"elem_num = %d\n",elem_num);
	fprintf(stderr,"curr->ssd_elem_num = %d\n",curr->ssd_elem_num);
	ASSERT(0);
   }*/
   elem = &currdisk->elements[elem_num];

   if ((x = ioqueue_physical_access_done(elem->queue,curr)) == NULL) {
      fprintf(stderr, "ssd_access_complete:  ioreq_event not found by ioqueue_physical_access_done call\n");
      exit(1);
   }

   // all the reqs are over
   if (ioqueue_get_reqoutstanding(elem->queue) == 0) {
    elem->media_busy = FALSE;
   }

   ssd_complete_parent(curr, currdisk);
   addtoextraq((event *) curr);
   ssd_activate_elem(currdisk, elem_num);
}

static void ssd_access_complete(ioreq_event *curr)
{
    ssd_t *currdisk = getssd (curr->devno);;

    switch(currdisk->params.alloc_pool_logic) {
        case SSD_ALLOC_POOL_PLANE:
        case SSD_ALLOC_POOL_CHIP:
            ssd_access_complete_element(curr);
        break;

        case SSD_ALLOC_POOL_GANG:
#if SYNC_GANG
            ssd_access_complete_gang_sync(curr);
#else
            ssd_access_complete_gang(curr);
#endif
        break;

        default:
            printf("Unknown alloc pool logic %d\n", currdisk->params.alloc_pool_logic);
            ASSERT(0);
    }
}

/* intermediate disconnect done */
static void ssd_disconnect_done (ioreq_event *curr)
{
   ssd_t *currdisk;

   currdisk = getssd (curr->devno);
   ssd_assert_current_activity(currdisk, curr);

   // fprintf (outputfile, "Entering ssd_disconnect for disk %d: %12.6f\n", currdisk->devno, simtime);

   addtoextraq((event *) curr);

   if (currdisk->busowned != -1) {
      bus_ownership_release(currdisk->busowned);
      currdisk->busowned = -1;
   }
   ssd_check_channel_activity (currdisk);
}

/* completion disconnect done */
static void ssd_completion_done (ioreq_event *curr)
{
   ssd_t *currdisk = getssd (curr->devno);
   ssd_assert_current_activity(currdisk, curr);

   // fprintf (outputfile, "Entering ssd_completion for disk %d: %12.6f\n", currdisk->devno, simtime);

   addtoextraq((event *) curr);

   if (currdisk->busowned != -1) {
      bus_ownership_release(currdisk->busowned);
      currdisk->busowned = -1;
   }

   ssd_check_channel_activity (currdisk);
}

static void ssd_interrupt_complete (ioreq_event *curr)
{
    // fprintf (outputfile, "j_debug Entered ssd_interrupt_complete - cause %d\n", curr->cause);

   switch (curr->cause) {

      case RECONNECT:
         ssd_reconnect_done(curr);
     break;

      case DISCONNECT:
     ssd_disconnect_done(curr);
     break;

      case COMPLETION:
     ssd_completion_done(curr);
     break;

      default:
         ddbg_assert2(0, "bad event type");
   }
}


void ssd_event_arrive (ioreq_event *curr)
{
   ssd_t *currdisk;

   currdisk = getssd (curr->devno);

   //fprintf (stderr, "Entered ssd_event_arrive: time %f (simtime %f)\n", curr->time, simtime);

   switch (curr->type) {

      case IO_ACCESS_ARRIVE:
         curr->time = simtime + currdisk->overhead;
         curr->type = DEVICE_OVERHEAD_COMPLETE;
         //curr->type = PUSH_EVENT_TO_USER_QUEUE;
         //addtointq((event *) curr);
	  vssd_push_event_to_user_queue(curr);
         break;

      case DEVICE_OVERHEAD_COMPLETE:
         ssd_request_arrive(curr);
         break;

      case DEVICE_ACCESS_COMPLETE:
         ssd_access_complete (curr);
         break;

      case DEVICE_DATA_TRANSFER_COMPLETE:
         ssd_bustransfer_complete(curr);
         break;

      case IO_INTERRUPT_COMPLETE:
         ssd_interrupt_complete(curr);
         break;

      case IO_QLEN_MAXCHECK:
         /* Used only at initialization time to set up queue stuff */
         curr->tempint1 = -1;
         curr->tempint2 = ssd_get_maxoutstanding(curr->devno);
         curr->bcount = 0;
         break;

      case SSD_CLEAN_GANG:
          ssd_clean_gang_complete(curr);
          break;

      case SSD_CLEAN_ELEMENT:
          ssd_clean_element_complete(curr);
          break;
//**********************************jian add************************************
      case USER_ALLOCATING:
	  vssd_initialize(curr);
	  break;

      case USER_DEALLOCATING:
	  vssd_deallocating(curr);
	  break;

      case CHOOSE_USER_TO_RUN_EVENT:
	  vssd_choose_user_to_run(curr);
	  break;

      case GIVING_PER_USER_QUOTA_EVENT:
	  vssd_giving_per_user_quota(curr);
	  break;
//**********************************jian add************************************

        default:
         fprintf(stderr, "Unrecognized event type at ssd_event_arrive\n");
         exit(1);
   }

   // fprintf (outputfile, "Exiting ssd_event_arrive\n");
}


int ssd_get_number_of_blocks (int devno)
{
   ssd_t *currdisk = getssd (devno);
   return (currdisk->numblocks);
}


int ssd_get_numcyls (int devno)
{
   ssd_t *currdisk = getssd (devno);
   return (currdisk->numblocks);
}


void ssd_get_mapping (int maptype, int devno, int blkno, int *cylptr, int *surfaceptr, int *blkptr)
{
   ssd_t *currdisk = getssd (devno);

   if ((blkno < 0) || (blkno >= currdisk->numblocks)) {
      fprintf(stderr, "Invalid blkno at ssd_get_mapping: %d\n", blkno);
      exit(1);
   }

   if (cylptr) {
      *cylptr = blkno;
   }
   if (surfaceptr) {
      *surfaceptr = 0;
   }
   if (blkptr) {
      *blkptr = 0;
   }
}


int ssd_get_avg_sectpercyl (int devno)
{
   return (1);
}


int ssd_get_distance (int devno, ioreq_event *req, int exact, int direction)
{
   /* just return an arbitrary constant, since acctime is constant */
   return 1;
}


// returning 0 to remove warning
double  ssd_get_servtime (int devno, ioreq_event *req, int checkcache, double maxtime)
{
   fprintf(stderr, "device_get_seektime not supported for ssd devno %d\n",  devno);
   assert(0);
   return 0;
}


// returning 0 to remove warning
double  ssd_get_acctime (int devno, ioreq_event *req, double maxtime)
{
   fprintf(stderr, "device_get_seektime not supported for ssd devno %d\n",  devno);
   assert(0);
   return 0;
}


int ssd_get_numdisks (void)
{
   return(numssds);
}


void ssd_cleanstats (void)
{
   int i, j;

   for (i=0; i<MAXDEVICES; i++) {
      ssd_t *currdisk = getssd (i);
      if (currdisk) {
          ioqueue_cleanstats(currdisk->queue);
          for (j=0; j<currdisk->params.nelements; j++)
              ioqueue_cleanstats(currdisk->elements[j].queue);
      }
   }
}

void ssd_setcallbacks ()
{
   ioqueue_setcallbacks();
}

int ssd_add(struct ssd *d) {
  int c;

  if(!disksim->ssdinfo) ssd_initialize_diskinfo();

  for(c = 0; c < disksim->ssdinfo->ssds_len; c++) {
    if(!disksim->ssdinfo->ssds[c]) {
      disksim->ssdinfo->ssds[c] = d;
      numssds++;
      return c;
    }
  }

  /* note that numdisks must be equal to diskinfo->disks_len */
  disksim->ssdinfo->ssds =
    realloc(disksim->ssdinfo->ssds,
        2 * c * sizeof(struct ssd *));

  bzero(disksim->ssdinfo->ssds + numssds,
    numssds);

  disksim->ssdinfo->ssds[c] = d;
  numssds++;
  disksim->ssdinfo->ssds_len *= 2;
  return c;
}


struct ssd *ssdmodel_ssd_loadparams(struct lp_block *b, int *num)
{
  /* temp vars for parameters */
  int n;
  struct ssd *result;

  if(!disksim->ssdinfo) ssd_initialize_diskinfo();

  result = malloc(sizeof(struct ssd));
  if(!result) return 0;
  bzero(result, sizeof(struct ssd));

  n = ssd_add(result);

  result->hdr = ssd_hdr_initializer;
  if(b->name)
    result->hdr.device_name = _strdup(b->name);

  lp_loadparams(result, b, &ssdmodel_ssd_mod);

  device_add((struct device_header *)result, n);
  if (num != NULL)
	  *num = n;
  return result;
}


struct ssd *ssd_copy(struct ssd *orig) {
  int i;
  struct ssd *result = malloc(sizeof(struct ssd));
  bzero(result, sizeof(struct ssd));
  memcpy(result, orig, sizeof(struct ssd));
  result->queue = ioqueue_copy(orig->queue);
  for (i=0;i<orig->params.nelements;i++)
      result->elements[i].queue = ioqueue_copy(orig->elements[i].queue);
  return result;
}

void ssd_set_syncset (int setstart, int setend)
{
}


static void ssd_acctime_printstats (int *set, int setsize, char *prefix)
{
   int i;
   statgen * statset[MAXDEVICES];

   if (device_printacctimestats) {
      for (i=0; i<setsize; i++) {
         ssd_t *currdisk = getssd (set[i]);
         statset[i] = &currdisk->stat.acctimestats;
      }
      stat_print_set(statset, setsize, prefix);
   }
}


static void ssd_other_printstats (int *set, int setsize, char *prefix)
{
   int i;
   int numbuswaits = 0;
   double waitingforbus = 0.0;

   for (i=0; i<setsize; i++) {
      ssd_t *currdisk = getssd (set[i]);
      numbuswaits += currdisk->stat.numbuswaits;
      waitingforbus += currdisk->stat.waitingforbus;
   }

   fprintf(outputfile, "%sTotal bus wait time: %f\n", prefix, waitingforbus);
   fprintf(outputfile, "%sNumber of bus waits: %d\n", prefix, numbuswaits);
}

void ssd_print_block_lifetime_distribution(int elem_num, ssd_t *s, int ssdno, double avg_lifetime, char *sourcestr)
{
    const int bucket_size = 20;
    int no_buckets = (100/bucket_size + 1);
    int i;
    int *hist;
    int dead_blocks = 0;
    int n;
    double sum;
    double sum_sqr;
    double mean;
    double variance;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    // allocate the buckets
    hist = (int *) malloc(no_buckets * sizeof(int));
    memset(hist, 0, no_buckets * sizeof(int));

    // to calc the variance
    n = s->params.blocks_per_element;
    sum = 0;
    sum_sqr = 0;

    for (i = 0; i < s->params.blocks_per_element; i ++) {
        int bucket;
        int rem_lifetime = metadata->block_usage[i].rem_lifetime;
        double perc = (rem_lifetime * 100.0) / avg_lifetime;

        // find out how many blocks have completely been erased.
        if (metadata->block_usage[i].rem_lifetime == 0) {
            dead_blocks ++;
        }

        if (perc >= 100) {
            // this can happen if a particular block was not
            // cleaned at all and so its remaining life time
            // is greater than the average life time. put these
            // blocks in the last bucket.
            bucket = no_buckets - 1;
        } else {
            bucket = (int) perc / bucket_size;
        }

        hist[bucket] ++;

        // calculate the variance
        sum = sum + rem_lifetime;
        sum_sqr = sum_sqr + (rem_lifetime*rem_lifetime);
    }


    fprintf(outputfile, "%s #%d elem #%d   ", sourcestr, ssdno, elem_num);
    fprintf(outputfile, "Block Lifetime Distribution\n");

    // print the bucket size
    fprintf(outputfile, "%s #%d elem #%d   ", sourcestr, ssdno, elem_num);
    for (i = bucket_size; i <= 100; i += bucket_size) {
        fprintf(outputfile, "< %d\t", i);
    }
    fprintf(outputfile, ">= 100\t\n");

    // print the histogram bar lengths
    fprintf(outputfile, "%s #%d elem #%d   ", sourcestr, ssdno, elem_num);
    for (i = bucket_size; i <= 100; i += bucket_size) {
        fprintf(outputfile, "%d\t", hist[i/bucket_size - 1]);
    }
    fprintf(outputfile, "%d\t\n", hist[no_buckets - 1]);

    mean = sum/n;
    variance = (sum_sqr - sum*mean)/(n - 1);
    fprintf(outputfile, "%s #%d elem #%d   Average of life time:\t%f\n",
        sourcestr, ssdno, elem_num, mean);
    fprintf(outputfile, "%s #%d elem #%d   Variance of life time:\t%f\n",
        sourcestr, ssdno, elem_num, variance);
    fprintf(outputfile, "%s #%d elem #%d   Total dead blocks:\t%d\n",
        sourcestr, ssdno, elem_num, dead_blocks);
}

//prints the cleaning algo statistics
void ssd_printcleanstats(int *set, int setsize, char *sourcestr)
{
    int i;
    int tot_ssd = 0;
    int elts_count = 0;
    double iops = 0;

    fprintf(outputfile, "\n\nSSD CLEANING STATISTICS\n");
    fprintf(outputfile, "---------------------------------------------\n\n");
    for (i = 0; i < setsize; i ++) {
        int j;
        int tot_elts = 0;
        ssd_t *s = getssd(set[i]);

        if (s->params.write_policy == DISKSIM_SSD_WRITE_POLICY_OSR) {

            elts_count += s->params.nelements;

            for (j = 0; j < s->params.nelements; j ++) {
                int plane_num;
                double avg_lifetime;
                double elem_iops = 0;
                double elem_clean_iops = 0;

                ssd_element_stat *stat = &(s->elements[j].stat);

                avg_lifetime = ssd_compute_avg_lifetime(-1, j, s);

                fprintf(outputfile, "%s #%d elem #%d   Total reqs issued:\t%d\n",
                    sourcestr, set[i], j, s->elements[j].stat.tot_reqs_issued);
                fprintf(outputfile, "%s #%d elem #%d   Total time taken:\t%f\n",
                    sourcestr, set[i], j, s->elements[j].stat.tot_time_taken);
                if (s->elements[j].stat.tot_time_taken > 0) {
                    elem_iops = ((s->elements[j].stat.tot_reqs_issued*1000.0)/s->elements[j].stat.tot_time_taken);
                    fprintf(outputfile, "%s #%d elem #%d   IOPS:\t%f\n",
                        sourcestr, set[i], j, elem_iops);
                }

                fprintf(outputfile, "%s #%d elem #%d   Total cleaning reqs issued:\t%d\n",
                    sourcestr, set[i], j, s->elements[j].stat.num_clean);
                fprintf(outputfile, "%s #%d elem #%d   Total cleaning time taken:\t%f\n",
                    sourcestr, set[i], j, s->elements[j].stat.tot_clean_time);
                fprintf(outputfile, "%s #%d elem #%d   Total migrations:\t%d\n",
                    sourcestr, set[i], j, s->elements[j].metadata.tot_migrations);
                fprintf(outputfile, "%s #%d elem #%d   Total pages migrated:\t%d\n",
                    sourcestr, set[i], j, s->elements[j].metadata.tot_pgs_migrated);
                fprintf(outputfile, "%s #%d elem #%d   Total migrations cost:\t%f\n",
                    sourcestr, set[i], j, s->elements[j].metadata.mig_cost);


                if (s->elements[j].stat.tot_clean_time > 0) {
                    elem_clean_iops = ((s->elements[j].stat.num_clean*1000.0)/s->elements[j].stat.tot_clean_time);
                    fprintf(outputfile, "%s #%d elem #%d   clean IOPS:\t%f\n",
                        sourcestr, set[i], j, elem_clean_iops);
                }

                fprintf(outputfile, "%s #%d elem #%d   Overall IOPS:\t%f\n",
                    sourcestr, set[i], j, ((s->elements[j].stat.num_clean+s->elements[j].stat.tot_reqs_issued)*1000.0)/(s->elements[j].stat.tot_clean_time+s->elements[j].stat.tot_time_taken));

                iops += elem_iops;

                fprintf(outputfile, "%s #%d elem #%d   Number of free blocks:\t%d\n",
                    sourcestr, set[i], j, s->elements[j].metadata.tot_free_blocks);
                fprintf(outputfile, "%s #%d elem #%d   Number of cleans:\t%d\n",
                    sourcestr, set[i], j, stat->num_clean);
                fprintf(outputfile, "%s #%d elem #%d   Pages moved:\t%d\n",
                    sourcestr, set[i], j, stat->pages_moved);
                fprintf(outputfile, "%s #%d elem #%d   Total xfer time:\t%f\n",
                    sourcestr, set[i], j, stat->tot_xfer_cost);
                if (stat->tot_xfer_cost > 0) {
                    fprintf(outputfile, "%s #%d elem #%d   Xfer time per page:\t%f\n",
                        sourcestr, set[i], j, stat->tot_xfer_cost/(1.0*stat->pages_moved));
                } else {
                    fprintf(outputfile, "%s #%d elem #%d   Xfer time per page:\t0\n",
                        sourcestr, set[i], j);
                }
                fprintf(outputfile, "%s #%d elem #%d   Average lifetime:\t%f\n",
                    sourcestr, set[i], j, avg_lifetime);
                fprintf(outputfile, "%s #%d elem #%d   Plane Level Statistics\n",
                    sourcestr, set[i], j);
                fprintf(outputfile, "%s #%d elem #%d   ", sourcestr, set[i], j);
                for (plane_num = 0; plane_num < s->params.planes_per_pkg; plane_num ++) {
                    fprintf(outputfile, "%d:(%d)  ",
                        plane_num, s->elements[j].metadata.plane_meta[plane_num].num_cleans);
                }
                fprintf(outputfile, "\n");


                ssd_print_block_lifetime_distribution(j, s, set[i], avg_lifetime, sourcestr);
                fprintf(outputfile, "\n");

                tot_elts += stat->pages_moved;
            }

            //fprintf(outputfile, "%s SSD %d average # of pages moved per element %d\n",
            //  sourcestr, set[i], tot_elts / s->params.nelements);

            tot_ssd += tot_elts;
            fprintf(outputfile, "\n");
        }
    }

    if (elts_count > 0) {
        fprintf(outputfile, "%s   Total SSD IOPS:\t%f\n",
            sourcestr, iops);
        fprintf(outputfile, "%s   Average SSD element IOPS:\t%f\n",
            sourcestr, iops/elts_count);
    }

    //fprintf(outputfile, "%s SSD average # of pages moved per ssd %d\n\n",
    //  sourcestr, tot_ssd / setsize);
}

void ssd_printsetstats (int *set, int setsize, char *sourcestr)
{
   int i;
   struct ioq * queueset[MAXDEVICES*SSD_MAX_ELEMENTS];
   int queuecnt = 0;
   int reqcnt = 0;
   char prefix[80];

   //using more secure functions
   sprintf_s4(prefix, 80, "%sssd ", sourcestr);
   for (i=0; i<setsize; i++) {
      ssd_t *currdisk = getssd (set[i]);
      struct ioq *q = currdisk->queue;
      queueset[queuecnt] = q;
      queuecnt++;
      reqcnt += ioqueue_get_number_of_requests(q);
   }
   if (reqcnt == 0) {
      fprintf (outputfile, "\nNo ssd requests for members of this set\n\n");
      return;
   }
   ioqueue_printstats(queueset, queuecnt, prefix);

   ssd_acctime_printstats(set, setsize, prefix);
   ssd_other_printstats(set, setsize, prefix);
}


void ssd_printstats (void)
{
   struct ioq * queueset[MAXDEVICES*SSD_MAX_ELEMENTS];
   int set[MAXDEVICES];
   int i,j;
   int reqcnt = 0;
   char prefix[80];
   int diskcnt;
   int queuecnt;

   fprintf(outputfile, "\nSSD STATISTICS\n");
   fprintf(outputfile, "---------------------\n\n");

   sprintf_s3(prefix, 80, "ssd ");

   diskcnt = 0;
   queuecnt = 0;
   for (i=0; i<MAXDEVICES; i++) {
      ssd_t *currdisk = getssd (i);
      if (currdisk) {
         struct ioq *q = currdisk->queue;
         queueset[queuecnt] = q;
         queuecnt++;
         reqcnt += ioqueue_get_number_of_requests(q);
         diskcnt++;
      }
   }
   assert (diskcnt == numssds);

   if (reqcnt == 0) {
      fprintf(outputfile, "No ssd requests encountered\n");
      return;
   }

   ioqueue_printstats(queueset, queuecnt, prefix);

   diskcnt = 0;
   for (i=0; i<MAXDEVICES; i++) {
      ssd_t *currdisk = getssd (i);
      if (currdisk) {
         set[diskcnt] = i;
         diskcnt++;
      }
   }
   assert (diskcnt == numssds);

   ssd_acctime_printstats(set, numssds, prefix);
   ssd_other_printstats(set, numssds, prefix);

   ssd_printcleanstats(set, numssds, prefix);

   fprintf (outputfile, "\n\n");

   for (i=0; i<numssds; i++) {
      ssd_t *currdisk = getssd (set[i]);
      if (currdisk->printstats == FALSE) {
          continue;
      }
      reqcnt = 0;
      {
          struct ioq *q = currdisk->queue;
          reqcnt += ioqueue_get_number_of_requests(q);
      }
      if (reqcnt == 0) {
          fprintf(outputfile, "No requests for ssd #%d\n\n\n", set[i]);
          continue;
      }
      fprintf(outputfile, "ssd #%d:\n\n", set[i]);
      sprintf_s4(prefix, 80, "ssd #%d ", set[i]);
      {
          struct ioq *q;
          q = currdisk->queue;
          ioqueue_printstats(&q, 1, prefix);
      }
      for (j=0;j<currdisk->params.nelements;j++) {
          char pprefix[100];
          struct ioq *q;
          sprintf_s5(pprefix, 100, "%s elem #%d ", prefix, j);
          q = currdisk->elements[j].queue;
          ioqueue_printstats(&q, 1, pprefix);
      }
      ssd_acctime_printstats(&set[i], 1, prefix);
      ssd_other_printstats(&set[i], 1, prefix);
      fprintf (outputfile, "\n\n");
   }
	//*jian_m printf VSSD per user state -start-
	sprintf_s3(prefix, 80, "j_debug ");
	fprintf(outputfile, "\nj_debug VSSD STATISTICS\n");
   	fprintf(outputfile, "j_debug ---------------------\n\n");
	fprintf(outputfile, "%s reqcnt = %d \n",prefix,reqcnt);
	ssd_t *currdisk = getssd (0);
	stat_print(&currdisk->queue->base.outtimestats,prefix);
	print_user();
	
	//*jian_m printf VSSD per user state -end-
}

// returning 0 to remove warning
double ssd_get_seektime (int devno,
                ioreq_event *req,
                int checkcache,
                double maxtime)
{
  fprintf(stderr, "device_get_seektime not supported for ssd devno %d\n",  devno);
  assert(0);
  return 0;
}

/* default ssd dev header */
struct device_header ssd_hdr_initializer = {
  DEVICETYPE_SSD,
  sizeof(struct ssd),
  "unnamed ssd",
  (void *)ssd_copy,
  ssd_set_depth,
  ssd_get_depth,
  ssd_get_inbus,
  ssd_get_busno,
  ssd_get_slotno,
  ssd_get_number_of_blocks,
  ssd_get_maxoutstanding,
  ssd_get_numcyls,
  ssd_get_blktranstime,
  ssd_get_avg_sectpercyl,
  ssd_get_mapping,
  ssd_event_arrive,
  ssd_get_distance,
  ssd_get_servtime,
  ssd_get_seektime,
  ssd_get_acctime,
  ssd_bus_delay_complete,
  ssd_bus_ownership_grant
};

//* jian Full live pages to user's allocating logical blocks
static void vssd_push_event_to_user_queue(ioreq_event *curr)
{
   /* verify that request is valid. */
	//fprintf(stderr,"vssd_push_event_to_user_queue (%f)",simtime);
		ioreq_event *ptr_event = curr;
		if(ptr_event->user_id == 0)
		{
			fprintf(outputfile,"j_debug curr->type==100 ptr_event->user_id == 0\n");
			assert(0);
		}
                user_time_state *ptr = find_user(ptr_event->user_id);
		ptr_event->start_service_time = simtime;
		ioreq_event_add_new_request(ptr->queue_per_user,ptr_event);

		total_waiting_requests_cnt++;
		ptr->waiting_request_cnt++;

		ioreq_event *new = getfromextraq();//add a even's head which about "reallocating to per user's time quota"
                new->time = simtime;
		new->type = CHOOSE_USER_TO_RUN_EVENT;
		//addtointq((event *)new);
		
		vssd_choose_user_to_run(new);
}
static void vssd_choose_user_to_run(ioreq_event *curr)
{
	//fprintf(stderr,"vssd_choose_user_to_run (%f)",simtime);
	addtoextraq((event *)curr);
	ssd_t *currdisk;
        currdisk = getssd (0);
	while(1)
	{
		if(total_waiting_requests_cnt == 0) return;

		user_time_state *ptr = user_time->next;
		ioreq_event *ptr_event = NULL;
		ioreq_event *temp_event = NULL;
		int temp_bcount = 0;
		int temp_flags = 0;
		int temp_user_id = 0;
		double temp_remain_credit_cost = 0;
		int round_robin_choosen = 0;
		while(ptr != user_time)
		{
			if(runing_request_cnt < MAX_DEVICE_REQUESTS_CNT_RUN_SAME_TIME)
			{
				if(ptr->curr_quota > 0 )
				{
					if(ptr->queue_per_user->prev != ptr->queue_per_user)
						temp_event = ptr->queue_per_user->prev;
					if(ptr->waiting_request_cnt > 0)
					{
						/*if(temp_event->user_id != ptr->user_id)
						{
							fprintf(stderr,"temp_event->user_id=%d\tptr->user_id=%d\n",temp_event->user_id,ptr->user_id);
						}*/
						//fprintf(outputfile,"j_debug (254) temp_event->bcount %d(user_id %d)\n",temp_event->bcount,ptr->user_id);
						if(temp_user_id == 0)
						{
							temp_bcount = temp_event->bcount;
							temp_user_id = temp_event->user_id;
							temp_flags = temp_event->flags;
							#ifndef ROUND_ROBIN
							temp_remain_credit_cost = ptr->curr_quota/((double)ptr->bandwidth/total_bandwidth);
							#endif
							if((round_robin+1) == temp_event->user_id)
								round_robin_choosen = 1;
						}
						#ifdef READ_FIRST-ROUND_ROBIN
						//********** Read First **************
						else if(temp_event->flags & READ)
						{
							if(temp_flags & READ)// Last:R  Current:R
							{
								if((round_robin+1) == temp_event->user_id)
								{
									temp_flags = temp_event->flags;
									temp_bcount = temp_event->bcount;
		                                                       	temp_user_id = temp_event->user_id;
									round_robin_choosen = 1;
								}
							}
							else// Last:W  Current:R
							{
								temp_flags = temp_event->flags;
								temp_bcount = temp_event->bcount;
	                                                        temp_user_id = temp_event->user_id;
							}
						}
						else
						{
							if(!(temp_flags & READ))// Last:W  Current:W
							{
								if((round_robin+1) == temp_event->user_id)
								{
									temp_flags = temp_event->flags;
									temp_bcount = temp_event->bcount;
		                                                       	temp_user_id = temp_event->user_id;
									round_robin_choosen = 1;
								}
							}
						}
						#elif defined ROUND_ROBIN
						//*********** Round Robin ****************
						else if((round_robin+1) == temp_event->user_id)
						{
							temp_flags = temp_event->flags;
							temp_bcount = temp_event->bcount;
		                                       	temp_user_id = temp_event->user_id;
							round_robin_choosen = 1;
						}
						#else
						//********** large Remain credit cost first ************
						if((ptr->curr_quota/((double)ptr->bandwidth/total_bandwidth)) > temp_remain_credit_cost)
						{
							temp_bcount = temp_event->bcount;
							temp_user_id = temp_event->user_id;
							temp_flags = temp_event->flags;
							temp_remain_credit_cost = ptr->curr_quota/((double)ptr->bandwidth/total_bandwidth);
						}
						#endif
						temp_event = NULL;
					}
				}
                                else // User has no remain credit cost
                                {
					if(((round_robin+1) == ptr->user_id)&& (round_robin_choosen == 0))
						round_robin = ((round_robin + 1) % user_count);
				#ifdef ALLOCATING_ELEMENT
 				#ifdef USE_SPARE_BAMDWIDTH
                                        ioreq_event *new = getfromextraq();//add a even's head which about "reallocating to per user's time quota"
                                        new->time = simtime;
                                        new->type = GIVING_PER_USER_QUOTA_EVENT;
                                        addtointq((event *)new);
                                #endif
                                #endif
                                }
			}
			ptr = ptr->next;
		}
		if(temp_user_id != 0)
		{
			ptr = find_user(temp_user_id);
			ptr_event = ioreq_event_get_next_request(ptr->queue_per_user);

			//ASSERT(ptr->curr_quota >= MAX_RESEPONCE_TIME);
			#ifdef VSD_QUANTA_BASED_SCHEDULER
			if(ptr->request_cnt > 0)
				ptr_event->spend_quota = ptr->response_time_VSD_FIOS / ptr->request_cnt;
			ASSERT(ptr_event->spend_quota >= 0);
			ptr->curr_quota -= ( ptr_event->spend_quota );
			ptr_event->start_time = simtime;//init
			#endif
			#ifdef FIOS_QUANTA_BASED_SCHEDULER
			if(ptr->request_cnt > 0)
				ptr_event->spend_quota = ptr->response_time_VSD_FIOS / ptr->request_cnt;
			ASSERT(ptr_event->spend_quota >= 0);
			ptr->curr_quota -= ( ptr_event->spend_quota );
			ptr_event->start_time = simtime;//init
			#endif

			ptr->waiting_request_cnt--;
        		total_waiting_requests_cnt--;
			runing_request_cnt ++;
			runing_request_bcount += ptr_event->bcount;
			ptr_event->time = simtime;
			if(round_robin_choosen == 1)
				round_robin = ((round_robin + 1) % user_count);
               	        addtointq((event *)ptr_event);
		}
		else
		{
			int flag = 0;
			int user_count = 0;
			user_time_state *ptr = user_time->next;
       	        	while(ptr != user_time)
               		{
				if(ptr->curr_quota > 0 )
				{
					if(ptr->waiting_request_cnt > 0)
					{
						flag = 1;
					}
				}
				user_count++;
				
                	        ptr = ptr->next;
	                }
			if(flag == 0)
			{
 			#ifdef USE_SPARE_BAMDWIDTH
				ioreq_event *new = getfromextraq();//add a even's head which about "reallocating to per user's time quota"
                		new->time = simtime;
				new->type = GIVING_PER_USER_QUOTA_EVENT;
				addtointq((event *)new);
				//vssd_giving_per_user_quota(curr);
			#endif
                                return;
			}
			else 
			{
                       	        //addtoextraq((event *)curr);
				return;
			}
		}
    	}
}
static void vssd_giving_per_user_quota(ioreq_event *curr)
{
	//fprintf(stderr,"vssd_giving_per_user_quota (%f)\n",simtime);
	//*jian giving per user allocaing bandwidth quota
	ssd_t *currdisk;
	currdisk = getssd (curr->devno);
	user_time_state *ptr = user_time->next;
	while(ptr != user_time)
	{
		ptr->last_quota = ptr->curr_quota;
		#ifdef VSSD_QUANTA_BASED_SCHEDULER
		ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)ptr->bandwidth/total_bandwidth) * currdisk->params.nelements;
		#endif
		#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY
		ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)ptr->bandwidth/total_bandwidth) * currdisk->params.nelements;
		#endif
		#ifdef VSSD_QUANTA_BASED_SCHEDULER_WITH_QUEUING_DELAY_SEPARATE_USER
		ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)ptr->bandwidth/total_bandwidth) * currdisk->params.nelements;
		#endif
		#ifdef VSD_QUANTA_BASED_SCHEDULER
		ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)ptr->bandwidth/total_bandwidth);
		#endif
		#ifdef FIOS_QUANTA_BASED_SCHEDULER
		ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)ptr->bandwidth/total_bandwidth);
		#endif
		if(ptr->last_quota < 0)
		ptr->curr_quota += ptr->last_quota;

		ptr=ptr->next;
	}
	last_reallocating_time = simtime;
	vssd_choose_user_to_run(curr);
}
static void vssd_finish_request(ioreq_event *curr)
{
	//if(curr->tempptr2 != NULL)
	//fprintf(stderr,"(%d) simtime %f curr->blkno %d curr->bcount %d\n",curr->user_id,simtime,curr->blkno,curr->bcount);
	ssd_t *currdisk;
	currdisk = getssd (curr->devno);
	#ifdef FIOS_QUANTA_BASED_SCHEDULER
	//FIOS_start_flag init value = 31
	if((FIOS_start_flag != 0) && (curr->blkno == 0 ) && (curr->user_id == 1 ))
	{
		if((curr->bcount == 8) && (curr->flags == 1) && (FIOS_start_flag & 1))//READ 8KB
		{
			fprintf(stderr,"simtime=%f\tcurr->blkno=%d\tcurr->bcount=%d\tcurr->flags=%d\n",simtime,curr->blkno,curr->bcount,curr->flags);
			FIOS_start_flag = FIOS_start_flag ^ 1;
			FIOS_init_v1 = (simtime - curr->start_time + currdisk->bus_transaction_latency);
			return;
		}
		if((curr->bcount == 256) && (curr->flags == 1) && (FIOS_start_flag & 2))//READ 256KB
		{
			fprintf(stderr,"simtime=%f\tcurr->blkno=%d\tcurr->bcount=%d\tcurr->flags=%d\n",simtime,curr->blkno,curr->bcount,curr->flags);
			FIOS_start_flag = FIOS_start_flag ^ 2;
			FIOS_init_v2 = (simtime - curr->start_time + currdisk->bus_transaction_latency);
			return;
		}
		if((curr->bcount == 8) && (curr->flags == 0) && (FIOS_start_flag & 4))//WRITE 8KB
		{
			fprintf(stderr,"simtime=%f\tcurr->blkno=%d\tcurr->bcount=%d\tcurr->flags=%d\n",simtime,curr->blkno,curr->bcount,curr->flags);
			FIOS_start_flag = FIOS_start_flag ^ 4;
			FIOS_init_v3 = (simtime - curr->start_time + currdisk->bus_transaction_latency);
			return;
		}
		if((curr->bcount == 256) && (curr->flags == 0) && (FIOS_start_flag & 8))//WRITE 256KB
		{
			fprintf(stderr,"simtime=%f\tcurr->blkno=%d\tcurr->bcount=%d\tcurr->flags=%d\n",simtime,curr->blkno,curr->bcount,curr->flags);
			FIOS_start_flag = FIOS_start_flag ^ 8;
			FIOS_init_v4 = (simtime - curr->start_time + currdisk->bus_transaction_latency);
			return;
		}
		if(FIOS_start_flag & 16)
		{
			FIOS_start_flag = FIOS_start_flag ^ 16;
			// y = ax + b
			FIOS_read_format_a = (FIOS_init_v2 - FIOS_init_v1)/(256-8);
			FIOS_read_format_b = FIOS_init_v2 - FIOS_read_format_a * 256;
			FIOS_write_format_a = (FIOS_init_v4 - FIOS_init_v3)/(256-8);
			FIOS_write_format_b = FIOS_init_v4 - FIOS_write_format_a * 256;
			//fprintf(stderr,"simtime=%f\tFIOS_read_format_a=%f\tFIOS_read_format_b=%f\nFIOS_write_format_a=%f\tFIOS_write_format_b=%f\n"\
			,simtime,FIOS_read_format_a,FIOS_read_format_b,FIOS_write_format_a,FIOS_write_format_b);
		}
	}
	#endif
	user_time_state *ptr;
	ptr=find_user(curr->user_id);
	if(ptr==NULL)
	{
		fprintf(stderr, "err add_new_user\ttime =%f\trequest_cnt=%d\tuser_id=%d\n",\
		user_time->prev->time,user_time->prev->request_cnt,user_time->prev->user_id);
	}
	else
	{
		if(ptr->user_id == 0)
			fprintf(outputfile, "j_debug err user updata time\tptr_user_id=%d\tcurr_user_id=%d\n",ptr->user_id,curr->user_id);
		ptr->time += (simtime - curr->start_service_time + currdisk->bus_transaction_latency);//add request response time
		ptr->request_cnt++;//add finished requests
		user_time->time += (simtime - curr->start_service_time + currdisk->bus_transaction_latency);//add total response time
		ptr->response_time_per_sec += (simtime - curr->start_service_time + currdisk->bus_transaction_latency);
		user_time->request_cnt++;
		simtime_f[curr->user_id]=simtime;

		if(max_response_time < (simtime - curr->start_service_time + currdisk->bus_transaction_latency))
			max_response_time = (simtime - curr->start_service_time + currdisk->bus_transaction_latency);
		//*jian for throuput per sec ,we want to know per user's throuput per sec
		ptr->blocks_per_sec += curr->bcount;
		ptr->total_finished_blocks += curr->bcount;
		ptr->ios_per_sec ++;
		user_time->blocks_per_sec += curr->bcount;

		runing_request_cnt --;
		runing_request_bcount -= curr->bcount;
		//*jian for user bandwidth ,the last quota should be restore
		double reseponse_time_temp = (simtime - curr->start_time + currdisk->bus_transaction_latency);
		#ifdef ACCESS_USER_BANDWIDTH

		#ifdef VSD_QUANTA_BASED_SCHEDULER
		ASSERT(curr->spend_quota >= 0);
		if(last_reallocating_time <= curr->start_time)
			ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_temp);
		else if(ptr->waiting_request_cnt > 0)
			ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_temp);
		
		ptr->response_time_VSD_FIOS += reseponse_time_temp;

		if(curr->start_time > simtime)
		{
			fprintf(stderr,"simtim %f curr->start_time%f\n",simtime,curr->start_time);
			ASSERT(0);
		}
		#endif
		#ifdef FIOS_QUANTA_BASED_SCHEDULER
		ASSERT(curr->spend_quota >= 0);
		double reseponse_time_of_format;
		if(curr->flags == 1)//READ
		{
			reseponse_time_of_format = FIOS_read_format_a * curr->bcount + FIOS_read_format_b;
		}
		if(curr->flags == 0)//WRITE
		{
			reseponse_time_of_format = FIOS_write_format_a * curr->bcount + FIOS_write_format_b;
		}
		double temp = reseponse_time_temp / reseponse_time_of_format;
		if((temp < FIOS_UP_BOUND) && (temp > FIOS_LOW_BOUND))
		{
			//fprintf(stderr,"simtime=%f\treseponse_time_temp=%f\treseponse_time_of_format=%f\n",simtime,reseponse_time_temp,reseponse_time_of_format);
			if(last_reallocating_time <= curr->start_time)
				ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_of_format);
			else if(ptr->waiting_request_cnt > 0)
				ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_of_format);
		}
		else 
		{
			//fprintf(stderr,"simtime=%f\treseponse_time_temp=%f\treseponse_time_of_format=%f\n",simtime,reseponse_time_temp,reseponse_time_of_format);
			if(last_reallocating_time <= curr->start_time)
				ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_temp / runing_request_cnt);
			else if(ptr->waiting_request_cnt > 0)
				ptr->curr_quota += (( curr->spend_quota ) - reseponse_time_temp / runing_request_cnt);
		}
		ptr->response_time_VSD_FIOS += reseponse_time_temp;
		#endif


		#ifdef ALLOCATING_ELEMENT
		element_run_flag[curr->user_id] = 0;
		#endif
		if(total_waiting_requests_cnt > 0)
                {
			ioreq_event *new = (ioreq_event *)getfromextraq();//add a even's head which about "reallocating to per user's time quota
	                new->time = simtime;
	                new->type = CHOOSE_USER_TO_RUN_EVENT;//*jian 254
	                addtointq(new);
		}
		total_finished_requests_cnt++;
		#endif
	}
}
static void vssd_initialize(ioreq_event *curr)
{
	//	(1)time (2)allocating/deallocating (3)user_id : (4)logical start block no (5)logical blocks size (6) real blocks size (7)bandwidths (%)
	fprintf(stderr,"%lf %d %d:%d %d %d %lf\n",\
	curr->time, curr->flags, curr->user_id, curr->blkno, curr->tempint1, curr->tempint2, curr->start_time);

	ssd_t *currdisk;
	currdisk = getssd (curr->devno);
	ssd_element_metadata *metadata;
	gang_metadata *g;

	int user_id;
	#ifdef CURREN_BLOCK_ENABLE
	user_id = curr->user_id;
	#endif
	#ifndef CURREN_BLOCK_ENABLE
	user_id = 1;
	#endif

	int i,j;
	//unsigned int lpn = curr->blkno * pages_per_block;//*jian first lpn address
	unsigned int lpn = curr->blkno * SSD_DATA_PAGES_PER_BLOCK(currdisk);//*jian first lpn address
	unsigned int lpn_size = curr->tempint1 * SSD_DATA_PAGES_PER_BLOCK(currdisk);//*jian vssd allocating page size
    	int ppage;
    	unsigned int bytes_to_alloc;
	unsigned int tot_blocks = currdisk->params.blocks_per_element;
    	unsigned int tot_pages = tot_blocks * currdisk->params.pages_per_block;
    	unsigned int reserved_blocks, usable_blocks, export_size;
    	unsigned int reserved_blocks_per_plane, usable_blocks_per_plane;
    	unsigned int bitpos;
    	unsigned int active_block;
    	unsigned int elem_index;
    	int plane_block_mapping = currdisk->params.plane_block_mapping;

        int pgnum_in_gang;
        int pp_index;
	int plane_num;
       	int block;
	int prev_pos;
	int elem_num;
    	//////////////////////////////////////////////////////////////////////////////
    	// active page starts at the 1st page on the reserved section
    	reserved_blocks_per_plane = (currdisk->params.reserve_blocks * currdisk->params.blocks_per_plane) / 100;
    	usable_blocks_per_plane = currdisk->params.blocks_per_plane - reserved_blocks_per_plane;
    	reserved_blocks = reserved_blocks_per_plane * currdisk->params.planes_per_pkg;
    	usable_blocks = usable_blocks_per_plane * currdisk->params.planes_per_pkg;


	#ifndef DEBUG_FIND_SSD_STATE
	fprintf(stderr,"GC save blocks=%d\n",currdisk->params.blocks_per_plane-((int)LOW_WATERMARK_PER_PLANE(currdisk)));
        fprintf(stderr,"logical blocks size=%d usable_blocks_per_plane=%d\n",curr->tempint1,usable_blocks_per_plane);
	//ASSERT(0);
	#endif

	//*********************** jian for VSSD which allocating with """"element"""" unit 	**********************
	#ifdef ALLOCATING_ELEMENT
	user_time_state *ptr;
	user_id = curr->user_id;
	unsigned int allocating_element_count = (curr->tempint1)/usable_blocks;
	unsigned int allocating_count = 0;
	fprintf(stderr,"user_id = %d  allocating_element_count = %d \n",user_id,allocating_element_count);


	ptr = find_user(user_id);

	ptr->element_allcating_count = allocating_element_count;
	ptr->init_element_no = allocating_element_ptr;
	i = allocating_element_ptr;

	while(1)
	{
		for(; i<currdisk->params.nelements ; i++)
		{
			metadata = &(currdisk->elements[i].metadata);
			if(metadata->element_user_id == 0)
			{
				metadata->element_user_id = user_id;
				allocating_count++;
				
				for(j=0; j<currdisk->params.planes_per_pkg; j++)
				{
					metadata->plane_meta[j].free_pages[user_id] += currdisk->params.blocks_per_plane * SSD_DATA_PAGES_PER_BLOCK(currdisk);
					metadata->plane_meta[j].free_pages[0] -= currdisk->params.blocks_per_plane * SSD_DATA_PAGES_PER_BLOCK(currdisk);
				}

				ASSERT(metadata->plane_meta[j].free_pages[0] >= 0);
			}
			if(allocating_element_count == allocating_count) break;
		}
		if(allocating_element_count == allocating_count) break;
		else
		i = 0;
	}
	if((i+1)==currdisk->params.nelements)
	{
		allocating_element_ptr = 0;
	}
	else
	{
		allocating_element_ptr = i+1;
	}
	/*//*debug
	if(user_id==1)
	{
	for(i=0; i<currdisk->params.nelements ; i++)
	{
		metadata = &(currdisk->elements[i].metadata);
		fprintf(stderr,"e=%d id=%d \t ",i,metadata->element_user_id);
		fprintf(stderr,"\n");
	}
	ASSERT(0);
	}
	//*debug*/

	export_size = usable_blocks * SSD_DATA_PAGES_PER_BLOCK(currdisk);
	for(j = ptr->init_element_no ; j<(ptr->element_allcating_count+ptr->init_element_no) ; j++ )
	{
    		ppage = 0;
		i = 0;
    		int elem_number = j % currdisk->params.nelements;
		elem_index = elem_number % currdisk->params.elements_per_gang;
		metadata = &(currdisk->elements[elem_number].metadata);
		metadata->user_id_2 = user_id;
		metadata->user_id = user_id;
		g = &currdisk->gang_meta[metadata->gang_num];
		while (i < export_size) {
        		int pgnum_in_gang;
        		int pp_index;
        		int plane_num;
        		unsigned int block = SSD_PAGE_TO_BLOCK(ppage, currdisk);

        		ASSERT(block < (unsigned int)currdisk->params.blocks_per_element);

        		// if this is the last page in the block
        		if (ssd_last_page_in_block(ppage, currdisk)) {
            			// leave this physical page for summary page and
            			// seal the block
            			metadata->block_usage[block].state = SSD_BLOCK_SEALED;

            			// go to next block
            			ppage ++;
            			block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
	
		    		plane_num = metadata->block_usage[block].plane_num;
		 		metadata->plane_meta[plane_num].free_blocks--;
				metadata->tot_free_blocks--;
        		}

        		// when the control comes here, 'ppage' contains the next page
        		// that can be assigned to a logical page.
        		// find the index of the phy page within the block
        		pp_index = ppage % currdisk->params.pages_per_block;

        		// populate the lba table
        		currdisk->page_level_mapping_table[lpn + (j-ptr->init_element_no) + (i*ptr->element_allcating_count)] = ppage 
			+ (metadata->element_number * currdisk->params.blocks_per_element * currdisk->params.pages_per_block);
        		pgnum_in_gang = elem_index * export_size + i;
        		g->pg2elem[pgnum_in_gang].e = elem_number;

        		// mark this block as not free and its state as 'in use'.
        		// note that a block could be not free and its state be 'sealed'.
        		// it is enough if we set it once while working on the first phy page.
        		// also increment the block sequence number.
        		if (pp_index == 0) {
            			bitpos = ssd_block_to_bitpos(currdisk, block);
            			ssd_set_bit(metadata->free_blocks, bitpos);
            			metadata->block_usage[block].state = SSD_BLOCK_INUSE;
        	    		metadata->block_usage[block].bsn[user_id] = metadata->bsn++;	//*jian modify
	    			metadata->block_usage[block].user_id = metadata->user_id;
        		}

        		// increase the usage count per block
        		plane_num = metadata->block_usage[block].plane_num;
        		metadata->block_usage[block].page[pp_index] = lpn + (j-ptr->init_element_no) + (i*ptr->element_allcating_count);
			#ifdef DEBUG_VSSD
			user_time_state *ptr_temp;
			ptr_temp = find_user(curr->user_id);
			metadata->block_usage[block].page_user_id[pp_index] = curr->user_id;
			ptr_temp->using_pages_cnt++;
			#endif
        		metadata->block_usage[block].num_valid ++;
        		metadata->plane_meta[plane_num].valid_pages ++;
			metadata->plane_meta[plane_num].free_pages[user_id] --;

        		// go to the next physical page
        		ppage ++;
			metadata->active_page[metadata->user_id] = ppage;//*jian add
        		metadata->plane_meta[plane_num].active_page[metadata->user_id] = ppage;

        		// go to the next logical page
        		i ++;
    		}
    	}
	return;
	#endif

	//*********************** jian for VSSD which allocating with """"PLANE"""" unit 	**********************
	#ifdef ALLOCATING_PLANE
	allocating_infor *ptr;
	user_time_state *ptr_2;
	user_id = curr->user_id;
	ptr_2 = find_user(user_id);
	unsigned int allocating_plane_count = (curr->tempint1)/usable_blocks_per_plane;
	unsigned int allocating_count = 0;

	allocating_infor_head[curr->user_id]->plane_allcating_count = allocating_plane_count;

	//fprintf(stderr,"logical blocks size=%d usable_blocks_per_plane=%d\n",curr->tempint1,usable_blocks_per_plane);
	//fprintf(stderr,"allocating_plane_count = %d\n",allocating_plane_count);

	i = allocating_element_ptr;
	j = allocating_plane_ptr;
	
	while(1)
	{
		for(; i<currdisk->params.nelements ; i++)
		{
			metadata = &(currdisk->elements[i].metadata);
			for(; j<currdisk->params.planes_per_pkg ; j++)
			{
				if(metadata->plane_meta[j].user_id == 0)
				{
					ptr = find_element(allocating_infor_head[curr->user_id],i);
					if(ptr==NULL)
					{
						ptr = allocating_infor_head[curr->user_id]->prev;
						ptr->next = malloc(sizeof(allocating_infor));
						ptr->next->element_no = i;
						ptr->next->plane_allcating_count = 0;
						ptr->next->start_page_no = allocating_page_no_per_element[i];
						allocating_infor_head[curr->user_id]->prev = ptr->next;
						ptr->next->next = allocating_infor_head[curr->user_id];
						ptr->next->prev = ptr;
						ptr = ptr->next;
					}
					if(ptr_2->init_elem_no == -1) ptr_2->init_elem_no = i;
					if(ptr_2->init_plane_no == -1) ptr_2->init_plane_no = j;
					//allocating_page_no_per_element[i] += usable_blocks_per_plane * currdisk->params.pages_per_block;
					allocating_page_no_per_element[i] += usable_blocks_per_plane * SSD_DATA_PAGES_PER_BLOCK(currdisk);
					ptr->plane_allcating_count++;
					metadata->plane_meta[j].user_id = curr->user_id;
					allocating_count++;

					metadata->plane_meta[j].free_pages[user_id] += currdisk->params.blocks_per_plane * SSD_DATA_PAGES_PER_BLOCK(currdisk);
					metadata->plane_meta[j].free_pages[0] -= currdisk->params.blocks_per_plane * SSD_DATA_PAGES_PER_BLOCK(currdisk);

					ASSERT(metadata->plane_meta[j].free_pages[0] >= 0);

					break;
				}
			}
			
			if(allocating_plane_count == allocating_count) break;
			else
			j = 0;
		}
		if(allocating_plane_count == allocating_count) break;
		else
		i = 0;
	}
	if((i+1)==currdisk->params.nelements)
	{
		allocating_element_ptr = 0;
		if((j+1) == currdisk->params.planes_per_pkg)
			allocating_plane_ptr = 0;
		else
			allocating_plane_ptr = j+1;
	}
	else
	{
		allocating_element_ptr = i+1;
		allocating_plane_ptr = j;
	}

	int plane_num_init[currdisk->params.nelements];
	for(i=0;i<currdisk->params.nelements;i++)
	{
		plane_num_init[i]=0;
	}

    	i = 0;
	j = 0;
    	export_size = lpn_size;
    	while (j < export_size ) {
		elem_num = (j % allocating_plane_count + ptr_2->init_elem_no) % currdisk->params.nelements;
		//elem_num =j%currdisk->params.nelements;

		ptr = find_element(allocating_infor_head[curr->user_id],elem_num);//*jian add

		metadata = &(currdisk->elements[elem_num].metadata);
    		elem_index = elem_num % currdisk->params.elements_per_gang;
		g = &currdisk->gang_meta[metadata->gang_num];
		metadata->user_id_2 = user_id;
		metadata->user_id = user_id;

		i = ((j/allocating_plane_count) * ptr->plane_allcating_count) + ((j%allocating_plane_count)/currdisk->params.nelements);
		if( ((j%allocating_plane_count)%currdisk->params.nelements) >= elem_num) i++;

    		ppage = metadata->active_page[user_id];

	        // if this is the last page in the block
        	if (ssd_last_page_in_block(ppage, currdisk)) {
	            // leave this physical page for summary page and
        	    // seal the block
		    if(ppage>-1)
		    {
        	    	block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
			metadata->block_usage[block].state = SSD_BLOCK_SEALED;
		    }
        	    // go to next block
	            ppage ++;
		    //block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
		    block = ssd_bitpos_to_block(metadata->block_alloc_pos, currdisk);
		    while(metadata->plane_meta[plane_num_init[elem_num]].user_id != user_id)
		    {
		    	plane_num_init[elem_num] = (plane_num_init[elem_num] + 1) % currdisk->params.planes_per_pkg;
		    }

		    prev_pos = metadata->plane_meta[plane_num_init[elem_num]].block_alloc_pos;

		    //alocating a block
		    block = -1;
		    bitpos = ssd_find_zero_bit(metadata->free_blocks, currdisk->params.blocks_per_element, prev_pos);
		    ASSERT(bitpos != -1);
		    block = ssd_bitpos_to_block(bitpos, currdisk);

		    ppage = block * currdisk->params.pages_per_block;

		    plane_num = metadata->block_usage[block].plane_num;
		    metadata->plane_meta[plane_num].free_blocks--;
		    metadata->tot_free_blocks--;
		    
		    metadata->plane_meta[plane_num_init[elem_num]].block_alloc_pos = \
            		(plane_num_init[elem_num] * currdisk->params.blocks_per_plane) + ((bitpos+1) % currdisk->params.blocks_per_plane);

		    plane_num_init[elem_num] = (plane_num_init[elem_num] + 1) % currdisk->params.planes_per_pkg;

		    metadata->block_alloc_pos = metadata->plane_meta[plane_num_init[elem_num]].block_alloc_pos;
	        }
        	    
		block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
		plane_num = metadata->block_usage[block].plane_num;

        	// when the control comes here, 'ppage' contains the next page
	        // that can be assigned to a logical page.
        	// find the index of the phy page within the block
	        pp_index = ppage % currdisk->params.pages_per_block;

        	// populate the lba table
        	currdisk->page_level_mapping_table[j+lpn] = ppage 
		+ (metadata->element_number * currdisk->params.blocks_per_element * currdisk->params.pages_per_block);

        	pgnum_in_gang = elem_index * usable_blocks * SSD_DATA_PAGES_PER_BLOCK(currdisk) + i;
        	//pgnum_in_gang = j;
	        g->pg2elem[pgnum_in_gang].e = elem_num;

        	// mark this block as not free and its state as 'in use'.
	        // note that a block could be not free and its state be 'sealed'.
        	// it is enough if we set it once while working on the first phy page.
	        // also increment the block sequence number.
        	if (pp_index == 0) {
	            bitpos = ssd_block_to_bitpos(currdisk, block);
        	    ssd_set_bit(metadata->free_blocks, bitpos);
	            metadata->block_usage[block].state = SSD_BLOCK_INUSE;
        	    metadata->block_usage[block].bsn[user_id] = metadata->bsn++;	//*jian modify
		    metadata->block_usage[block].user_id = user_id;
        	}

	        // increase the usage count per block
        	plane_num = metadata->block_usage[block].plane_num;
	        metadata->block_usage[block].page[pp_index] = j+lpn;
		#ifdef DEBUG_VSSD
		user_time_state *ptr_temp;
		ptr_temp = find_user(curr->user_id);
		metadata->block_usage[block].page_user_id[pp_index] = curr->user_id;
		ptr_temp->using_pages_cnt++;
		#endif
        	metadata->block_usage[block].num_valid ++;
	        metadata->plane_meta[plane_num].valid_pages ++;
		metadata->plane_meta[plane_num].free_pages[user_id] --;

        	// go to the next physical page
	        ppage ++;
		metadata->active_page[user_id] = ppage;//*jian add
		metadata->plane_meta[plane_num].active_page[user_id] = ppage;

        	// go to the next logical page
		j ++;
    	}
	return;
	#endif

	//*********************** jian (1) for normal SSD 					**********************
	//*********************** jian (2) for VSSD which allocating with """"BLOCK"""" unit 	**********************
	#ifdef CURREN_BLOCK_ENABLE
	#ifdef GC_SELF_BLOCKS
	j=allocating_blk_ptr;
	for(;j<(curr->tempint2+allocating_blk_ptr);j++)
	{
		elem_num =j%currdisk->params.nelements;
                metadata = &(currdisk->elements[elem_num].metadata);
		i = j/currdisk->params.nelements;
		plane_num = i%currdisk->params.planes_per_pkg;
		metadata->plane_meta[plane_num].user_allocating_blocks[user_id]++;

		metadata->plane_meta[plane_num].free_pages[0] -= SSD_DATA_PAGES_PER_BLOCK(currdisk);
		metadata->plane_meta[plane_num].free_pages[user_id] += SSD_DATA_PAGES_PER_BLOCK(currdisk);

		ASSERT(metadata->plane_meta[plane_num].free_pages[0] >= 0);
	}
	allocating_blk_ptr =j;
	#endif
	#endif
	#ifndef ALLOCATING_ELEMENT
	#ifndef ALLOCATING_PLANE
	user_time_state *ptr;
	ptr = find_user(curr->user_id);
    	i = 0;
	j = lpn;
    	export_size = lpn_size;
    	while (j < (export_size+lpn) ) {		
		elem_num =j%currdisk->params.nelements;
		if(j==((83445448/8)))
		{
			fprintf(stderr,"init 83445448/8 elem_num = %d\n",elem_num);
		}
		metadata = &(currdisk->elements[elem_num].metadata);
    		elem_index = elem_num % currdisk->params.elements_per_gang;
		g = &currdisk->gang_meta[metadata->gang_num];
		metadata->user_id_2 = user_id;
		metadata->user_id = user_id;

		i = j/currdisk->params.nelements;
    		ppage = metadata->active_page[user_id];

	        // if this is the last page in the block
        	if (ssd_last_page_in_block(ppage, currdisk)) {
	            // leave this physical page for summary page and
        	    // seal the block
		    if(ppage>-1)
		    {
        	    	block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
			metadata->block_usage[block].state = SSD_BLOCK_SEALED;
		    }
        	    // go to next block
	            ppage ++;
		    //block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
		    block = ssd_bitpos_to_block(metadata->block_alloc_pos, currdisk);
		    plane_num = metadata->block_usage[block].plane_num;
		    prev_pos = metadata->plane_meta[plane_num].block_alloc_pos;

		    //alocating a block
		    block = -1;
		    bitpos = ssd_find_zero_bit(metadata->free_blocks, currdisk->params.blocks_per_element, prev_pos);
		    block = ssd_bitpos_to_block(bitpos, currdisk);
		    ASSERT(block != -1);

		    ppage = block * currdisk->params.pages_per_block;
		    
		    metadata->plane_meta[plane_num].block_alloc_pos = \
            		(plane_num * currdisk->params.blocks_per_plane) + ((bitpos+1) % currdisk->params.blocks_per_plane);
		    metadata->block_alloc_pos = metadata->plane_meta[(plane_num+1)%currdisk->params.planes_per_pkg].block_alloc_pos;
		    #ifdef CURREN_BLOCK_ENABLE
		    #ifdef GC_SELF_BLOCKS
		    metadata->user_using_blocks[user_id]++;
		    metadata->plane_meta[plane_num].user_using_blocks[user_id]++;
		    #endif
		    #endif
		    metadata->plane_meta[plane_num].free_blocks--;
		    metadata->tot_free_blocks--;
	        }
        	    
		block = SSD_PAGE_TO_BLOCK(ppage, currdisk);
		plane_num = metadata->block_usage[block].plane_num;

        	// when the control comes here, 'ppage' contains the next page
	        // that can be assigned to a logical page.
        	// find the index of the phy page within the block
	        pp_index = ppage % currdisk->params.pages_per_block;

        	// populate the lba table
        	currdisk->page_level_mapping_table[j] = ppage 
		+ (metadata->element_number * currdisk->params.blocks_per_element * currdisk->params.pages_per_block);
        	pgnum_in_gang = elem_index * usable_blocks * SSD_DATA_PAGES_PER_BLOCK(currdisk) + i;
        	//pgnum_in_gang = j;
	        g->pg2elem[pgnum_in_gang].e = elem_num;

        	// mark this block as not free and its state as 'in use'.
	        // note that a block could be not free and its state be 'sealed'.
        	// it is enough if we set it once while working on the first phy page.
	        // also increment the block sequence number.
        	if (pp_index == 0) {
	            bitpos = ssd_block_to_bitpos(currdisk, block);
        	    ssd_set_bit(metadata->free_blocks, bitpos);
	            metadata->block_usage[block].state = SSD_BLOCK_INUSE;
        	    metadata->block_usage[block].bsn[user_id] = metadata->bsn++;	//*jian modify
		    metadata->block_usage[block].user_id = user_id;
        	}

	        // increase the usage count per block
        	plane_num = metadata->block_usage[block].plane_num;
	        metadata->block_usage[block].page[pp_index] = j;
		#ifdef DEBUG_VSSD
		user_time_state *ptr_temp;
		ptr_temp = find_user(curr->user_id);
		metadata->block_usage[block].page_user_id[pp_index] = curr->user_id;
		//ptr_temp->using_pages_cnt++;
		#endif
		// hu modify
		ptr->using_pages_cnt ++;
		ptr->using_pages_per_sec ++;
		ptr->live_page_cnt ++;
		ptr->live_page_per_sec ++;		
		/////////////////////////        	
		metadata->block_usage[block].num_valid ++;
	        metadata->plane_meta[plane_num].valid_pages ++;
		metadata->plane_meta[plane_num].free_pages[user_id] --;

        	// go to the next physical page
	        ppage ++;
		metadata->active_page[user_id] = ppage;//*jian add
		metadata->plane_meta[plane_num].active_page[user_id] = ppage;

        	// go to the next logical page
		j ++;
    	}
	/*if(user_id ==2)
	{
		fprintf(stderr,"i = %d\n",i);
		ASSERT(0);
	}*/
	return;
	#endif
	#endif
	
}
static void vssd_deallocating(ioreq_event *curr)
{
	//	(1)time (2)allocating/deallocating (3)user_id : (4)logical start block no (5)logical blocks size (6) real blocks size (7)bandwidths (%)
	fprintf(stderr,"%lf %d %d:%d %d %d %lf\n",\
	curr->time, curr->flags, curr->user_id, curr->blkno, curr->tempint1, curr->tempint2, curr->start_time);

	ssd_t *currdisk;
	currdisk = getssd (curr->devno);
	ssd_element_metadata *metadata;
	gang_metadata *g;

	int j,i;
	int user_id = curr->user_id;
	unsigned int total_blocks = currdisk->params.blocks_per_element;
    	/*unsigned int reserved_blocks, usable_blocks;
    	unsigned int reserved_blocks_per_plane, usable_blocks_per_plane;
    	reserved_blocks_per_plane = (currdisk->params.reserve_blocks * currdisk->params.blocks_per_plane) / 100;
    	usable_blocks_per_plane = currdisk->params.blocks_per_plane - reserved_blocks_per_plane;
    	reserved_blocks = reserved_blocks_per_plane * currdisk->params.planes_per_pkg;
    	usable_blocks = usable_blocks_per_plane * currdisk->params.planes_per_pkg;*/
	
	#ifdef CURREN_BLOCK_ENABLE
	for(j = 0 ; j<currdisk->params.nelements ; j++ )
	{
		metadata = &(currdisk->elements[j].metadata);
		for(i = 0; i<total_blocks ; i++)
		{
			int plane_num = metadata->block_usage[i].plane_num;
			if(metadata->block_usage[i].user_id == user_id)
			{
				#ifdef ALLOCATING_PLANE
				metadata->plane_meta[plane_num].user_id = 0;
				allocating_infor_head[user_id]->prev = allocating_infor_head[user_id];
				allocating_infor_head[user_id]->next = allocating_infor_head[user_id];
				allocating_infor_ptr[user_id] = allocating_infor_head[user_id];
				#endif
				#ifdef ALLOCATING_ELEMENT
				metadata->element_user_id = 0;
				#endif
				if((metadata->block_usage[i].state == SSD_BLOCK_SEALED) ||
			   	(metadata->block_usage[i].state == SSD_BLOCK_INUSE)	)
				{
					int lpn,pp;
					for(pp = 0;pp < currdisk->params.pages_per_block;pp++)
					{
						lpn = metadata->block_usage[i].page[pp];
						if(lpn != -1)
						{
							currdisk->page_level_mapping_table[lpn] = -1; 
							metadata->block_usage[i].num_valid --;
							metadata->plane_meta[plane_num].valid_pages --;
							metadata->block_usage[i].page[pp] = -1;
						}
					}
					metadata->block_usage[i].state = SSD_BLOCK_SEALED;
					ASSERT(metadata->block_usage[i].num_valid == 0);
				}
				metadata->block_usage[i].user_id = 0;
			}
			metadata->active_page[user_id] = -1;//*jian add
                	metadata->plane_meta[plane_num].active_page[user_id] = -1;
		}
	}
	#endif
}
//*jian add-end
