/*
 * DiskSim Storage Subsystem Simulation Environment (Version 4.0)
 * Revision Authors: John Bucy, Greg Ganger
 * Contributors: John Griffin, Jiri Schindler, Steve Schlosser
 *
 * Copyright (c) of Carnegie Mellon University, 2001-2008.
 *
 * This software is being provided by the copyright holders under the
 * following license. By obtaining, using and/or copying this software,
 * you agree that you have read, understood, and will comply with the
 * following terms and conditions:
 *
 * Permission to reproduce, use, and prepare derivative works of this
 * software is granted provided the copyright and "No Warranty" statements
 * are included with all reproductions and derivative works and associated
 * documentation. This software may also be redistributed without charge
 * provided that the copyright and "No Warranty" statements are included
 * in all redistributions.
 *
 * NO WARRANTY. THIS SOFTWARE IS FURNISHED ON AN "AS IS" BASIS.
 * CARNEGIE MELLON UNIVERSITY MAKES NO WARRANTIES OF ANY KIND, EITHER
 * EXPRESSED OR IMPLIED AS TO THE MATTER INCLUDING, BUT NOT LIMITED
 * TO: WARRANTY OF FITNESS FOR PURPOSE OR MERCHANTABILITY, EXCLUSIVITY
 * OF RESULTS OR RESULTS OBTAINED FROM USE OF THIS SOFTWARE. CARNEGIE
 * MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
 * TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
 * COPYRIGHT HOLDERS WILL BEAR NO LIABILITY FOR ANY USE OF THIS SOFTWARE
 * OR DOCUMENTATION.
 *
 */



/*
 * DiskSim Storage Subsystem Simulation Environment (Version 2.0)
 * Revision Authors: Greg Ganger
 * Contributors: Ross Cohen, John Griffin, Steve Schlosser
 *
 * Copyright (c) of Carnegie Mellon University, 1999.
 *
 * Permission to reproduce, use, and prepare derivative works of
 * this software for internal use is granted provided the copyright
 * and "No Warranty" statements are included with all reproductions
 * and derivative works. This software may also be redistributed
 * without charge provided that the copyright and "No Warranty"
 * statements are included in all redistributions.
 *
 * NO WARRANTY. THIS SOFTWARE IS FURNISHED ON AN "AS IS" BASIS.
 * CARNEGIE MELLON UNIVERSITY MAKES NO WARRANTIES OF ANY KIND, EITHER
 * EXPRESSED OR IMPLIED AS TO THE MATTER INCLUDING, BUT NOT LIMITED
 * TO: WARRANTY OF FITNESS FOR PURPOSE OR MERCHANTABILITY, EXCLUSIVITY
 * OF RESULTS OR RESULTS OBTAINED FROM USE OF THIS SOFTWARE. CARNEGIE
 * MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
 * TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
 */

/*
 * DiskSim Storage Subsystem Simulation Environment
 * Authors: Greg Ganger, Bruce Worthington, Yale Patt
 *
 * Copyright (C) 1993, 1995, 1997 The Regents of the University of Michigan 
 *
 * This software is being provided by the copyright holders under the
 * following license. By obtaining, using and/or copying this software,
 * you agree that you have read, understood, and will comply with the
 * following terms and conditions:
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose and without fee or royalty is
 * hereby granted, provided that the full text of this NOTICE appears on
 * ALL copies of the software and documentation or portions thereof,
 * including modifications, that you make.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS," AND COPYRIGHT HOLDERS MAKE NO
 * REPRESENTATIONS OR WARRANTIES, EXPRESS OR IMPLIED. BY WAY OF EXAMPLE,
 * BUT NOT LIMITATION, COPYRIGHT HOLDERS MAKE NO REPRESENTATIONS OR
 * WARRANTIES OF MERCHANTABILITY OR FITNESS FOR ANY PARTICULAR PURPOSE OR
 * THAT THE USE OF THE SOFTWARE OR DOCUMENTATION WILL NOT INFRINGE ANY
 * THIRD PARTY PATENTS, COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS. COPYRIGHT
 * HOLDERS WILL BEAR NO LIABILITY FOR ANY USE OF THIS SOFTWARE OR
 * DOCUMENTATION.
 *
 *  This software is provided AS IS, WITHOUT REPRESENTATION FROM THE
 * UNIVERSITY OF MICHIGAN AS TO ITS FITNESS FOR ANY PURPOSE, AND
 * WITHOUT WARRANTY BY THE UNIVERSITY OF MICHIGAN OF ANY KIND, EITHER
 * EXPRESSED OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE REGENTS
 * OF THE UNIVERSITY OF MICHIGAN SHALL NOT BE LIABLE FOR ANY DAMAGES,
 * INCLUDING SPECIAL , INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
 * WITH RESPECT TO ANY CLAIM ARISING OUT OF OR IN CONNECTION WITH THE
 * USE OF OR IN CONNECTION WITH THE USE OF THE SOFTWARE, EVEN IF IT HAS
 * BEEN OR IS HEREAFTER ADVISED OF THE POSSIBILITY OF SUCH DAMAGES
 *
 * The names and trademarks of copyright holders or authors may NOT be
 * used in advertising or publicity pertaining to the software without
 * specific, written prior permission. Title to copyright in this software
 * and any associated documentation will at all times remain with copyright
 * holders.
 */

#include "disksim_global.h"
#include "disksim_ioface.h"
#include "disksim_pfface.h"
#include "disksim_iotrace.h"
#include "config.h"

#include "modules/disksim_global_param.h"

#include <stdio.h>
#include <signal.h>
#include <stdarg.h>

#ifdef SUPPORT_CHECKPOINTS
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif


disksim_t *disksim = NULL;

/* legacy hack for HPL traces... */
#define PRINT_TRACEFILE_HEADER	FALSE


/*** Functions to allocate and deallocate empty event structures ***/

/* Creates new, empty events (using malloc) and puts them on the extraq. */
/* Called by getfromextraq when the queue is found to be empty.          */

#define EXTRAEVENTS 32

struct user_time_state *find_user(int);
void add_user(int , int , int , int , double);
void del_user(int );
void print_user(void);

#ifdef ALLOCATING_PLANE
struct allocating_infor * find_element( allocating_infor *, int);
struct allocating_infor * find_element( allocating_infor *head, int element_no)
{
	allocating_infor *ptr = head->next;
	while(ptr!=head)
	{
		if(ptr->element_no == element_no)
			return ptr;
		ptr=ptr->next;
	}
	return NULL;
}
#endif

//*jian input user information tool -start
struct user_time_state *find_user(int new_user_id)
{
        user_time_state *ptr = user_time->next;
        while(ptr!=user_time)
        {
                if(ptr->user_id == new_user_id)
                        return ptr;
                ptr=ptr->next;
        }
        return NULL;
}
//*link list add "new user" to "user_time(link list head)" prev
void add_user(int new_user_id, int first_log_blk_address, int user_request_log_space, int user_request_real_space, double user_request_bandwidth)
{
        if(find_user(new_user_id) != NULL)
        {
                fprintf(stderr, "Add same new user_id = %d\n", new_user_id);
                assert(0);
        }

        user_time_state *new_ptr = malloc(sizeof(user_time_state));
        new_ptr->time = 0;
        new_ptr->request_cnt = 0;
	new_ptr->gc_overhead_time = 0.0;
	new_ptr->gc_overhead_cnt = 0;
	new_ptr->gc_issues_cnt = 0;
	// hu init
	new_ptr->live_page_cnt = 0;
	new_ptr->live_page_per_sec = 0;
	new_ptr->using_pages_per_sec = 0;
	new_ptr->gc_live_page_copy_per_sec = 0;
	new_ptr->gc_issues_per_sec = 0;
	///////////////////////
        new_ptr->user_id = new_user_id;
	#ifdef ALLOCATING_PLANE
	new_ptr->init_elem_no = -1;
	new_ptr->init_plane_no = -1; 
	#endif
	#ifdef DEBUG_VSSD
	new_ptr->using_pages_cnt = 0;	
	#endif

        user_time->bandwidth -= user_request_bandwidth;
        user_time->aloc_block_cnt -= user_request_real_space;
        user_time->aloc_log_block_cnt -= user_request_log_space;
	user_time->free_pool_block_cnt = user_time->aloc_block_cnt; //hu,remain block in SSD

	if(user_time->bandwidth<0 || user_time->aloc_block_cnt<0)
	{
		fprintf(stderr,"Allocating Error!!\n");
		fprintf(stderr,"free_bandwidth = %d\n",user_time->bandwidth);
		fprintf(stderr,"free_block_cnt = %d\n",user_time->aloc_block_cnt);
        	//assert(0);
	}
	//Jian assign SLO
        new_ptr->bandwidth = user_request_bandwidth;
        new_ptr->aloc_block_cnt = user_request_real_space;
        new_ptr->aloc_log_block_cnt = user_request_log_space;
	new_ptr->first_log_blk_address = first_log_blk_address;

	new_ptr->using_block_cnt = user_request_log_space;//*jian init full logical blocks

	//new_ptr->using_block_cnt = 0;//*jian real simulate
	new_ptr->using_spare_block_cnt = 0;

        new_ptr->next = user_time;
        new_ptr->prev = user_time->prev;
        user_time->prev->next = new_ptr;
        user_time->prev = new_ptr;

	new_ptr->last_quota = 0;
	new_ptr->response_time_VSD_FIOS = 0;
        //new_ptr->curr_quota = ALLOCATING_TIME_UNIT * ((double)new_ptr->bandwidth/100);

	new_ptr->queue_per_user = malloc(sizeof(ioreq_event));
	new_ptr->queue_per_user->next = new_ptr->queue_per_user;
	new_ptr->queue_per_user->prev = new_ptr->queue_per_user;

	new_ptr->waiting_request_cnt = 0;
	new_ptr->blocks_per_sec = 0;
	new_ptr->total_clean_time_per_sec = 0;
	new_ptr->response_time_per_sec = 0;
	new_ptr->total_finished_blocks = 0;
	new_ptr->ios_per_sec = 0;

	user_count++;

	if(total_bandwidth == 0)
	total_bandwidth = user_request_bandwidth;
	else total_bandwidth += user_request_bandwidth;
	#ifndef USE_SPARE_BAMDWIDTH
	total_bandwidth = 100;
	#endif
        return;
}
void del_user(int del_user_id)
{
        user_time_state *del_ptr;
        if( (del_ptr = find_user(del_user_id)) == NULL)
        {
                fprintf(stderr, "Delete user which not find (user_id = %d)\n", del_user_id);
                assert(0);
        }
	else
	print_user();

        del_ptr->time = 0.0;
        del_ptr->request_cnt = 0;
        del_ptr->user_id = 0;

        del_ptr->next->prev = del_ptr->prev;
        del_ptr->prev->next = del_ptr->next;

        free (del_ptr);
        return;
}
void print_user(void)
{
        int total_cnt = 0;
        double total_time = 0.0;
        user_time_state *ptr = user_time->next;
	double temp1,temp2;
        while(ptr!=user_time)
        {
                double temp=0;
                if(ptr->request_cnt!=0)
                        temp=ptr->time/ptr->request_cnt;
                fprintf(outputfile,"j_debug User(%d)-completion request cnt=%d\tresponce time = %f\n",\
                ptr->user_id,ptr->request_cnt,temp);
        	fprintf(outputfile,"j_debug User(%d)-GC overhead time (Total) : %f \n",ptr->user_id,ptr->gc_overhead_time);
        	fprintf(outputfile,"j_debug User(%d)-GC overhead cnt (Total) : %d \n",ptr->user_id,ptr->gc_overhead_cnt);
        	fprintf(outputfile,"j_debug User(%d)-GC issues cnt (Total) : %d \n",ptr->user_id,ptr->gc_issues_cnt);
		temp1 = ptr->total_finished_blocks;
		temp2 = simtime_f[ptr->user_id]*2.048;
        	fprintf(outputfile,"j_debug User(%d)-Avg throughput(MB/s) : %f (total blk =%d simtime = %f)\n",\
		ptr->user_id,temp1/temp2,ptr->total_finished_blocks,simtime_f[ptr->user_id]);
                total_cnt += ptr->request_cnt;
                total_time += ptr->time;
                ptr=ptr->next;
        }
        fprintf(outputfile,"j_debug Total -completion request cnt=%d\tresponce time = %f\n",\
                total_cnt, user_time->time / user_time->request_cnt );
	temp1 = user_time->blocks_per_sec;
        //fprintf(outputfile,"j_debug Avg throughpu = %f MB/s (total blk =%d simtime = %f)\n",temp1/temp2,user_time->blocks_per_sec,simtime);
        fprintf(outputfile,"j_debug Max response time : %f \n",max_response_time);
        return NULL;

}


void ioreq_event_add_new_request(ioreq_event *queue_head, ioreq_event *curr)
{
	curr->prev = queue_head;
	curr->next = queue_head->next;
	queue_head->next->prev = curr;
	queue_head->next = curr;
}

ioreq_event *ioreq_event_get_next_request(ioreq_event *queue_head)
{
	if(queue_head->prev == queue_head)return NULL;
	ioreq_event *ptr = queue_head->prev;

	queue_head->prev->prev->next = queue_head;
	queue_head->prev = queue_head->prev->prev;
	ptr->prev = NULL;
	ptr->next=NULL;
	return ptr;
}


//*jian input user information tool -end

static void allocateextra ()
{
   int i;
   event *temp = NULL;

   StaticAssert (sizeof(event) == DISKSIM_EVENT_SIZE);

   temp = calloc(EXTRAEVENTS, sizeof(event));

   //   if ((temp = (event *)DISKSIM_malloc(ALLOCSIZE)) == NULL) {

   ddbg_assert(temp != 0);

   //   for (i=0; i<((ALLOCSIZE/DISKSIM_EVENT_SIZE)-1); i++) {

   for(i = 0; i < EXTRAEVENTS; i++) {
      temp[i].next = &temp[i+1];
   }
   temp[EXTRAEVENTS-1].next = disksim->extraq;
   disksim->extraq = temp;
   disksim->extraqlen += EXTRAEVENTS;
   //   disksim->extraqlen = ALLOCSIZE / DISKSIM_EVENT_SIZE;
}

/* A simple check to make sure that you're not adding an event
   to the extraq that is already there! */

int addtoextraq_check(event *ev)
{
    event *temp = disksim->extraq;

    while (temp != NULL) {
	if (ev == temp) {
	  // I did it this way so that I could break at this line -schlos
	    ddbg_assert(ev != temp);
	}
	temp = temp->next;
    }
    return 1;
}

/* Deallocates an event structure, adding it to the extraq free pool. */
  
INLINE void addtoextraq (event *temp)
{
  // addtoextraq_check(temp);

   if (temp == NULL) {
      return;
   }
   temp->next = disksim->extraq;
   temp->prev = NULL;
   disksim->extraq = temp;
   disksim->extraqlen++;
}


/* Allocates an event structure from the extraq free pool; if empty, */
/* calls allocateextra to create some more.                          */

INLINE event * getfromextraq ()
{
  event *temp;

  if(disksim->extraqlen == 0) {
    allocateextra();
    temp = disksim->extraq;
    disksim->extraq = disksim->extraq->next;
  }
  else if(disksim->extraqlen == 1) {
    temp = disksim->extraq;
    disksim->extraq = NULL;
  }
  else {
    temp = disksim->extraq;
    disksim->extraq = disksim->extraq->next;
  }

  disksim->extraqlen--;
  temp->next = NULL;
  temp->prev = NULL;
  return temp;
}


/* Deallocates a list of event structures to the extraq free pool. */

void addlisttoextraq (event **headptr)
{
   event *tmp1, *tmp2;

   tmp1 = *headptr;
   if(!tmp1) return;
   
/*     while ((tmp = *headptr)) { */
/*        *headptr = tmp->next; */
/*        addtoextraq(tmp); */
/*     } */


   do {
     tmp2 = tmp1->next;
     addtoextraq(tmp1);
     tmp1 = tmp2;
   } while(tmp1 && (tmp1 != (*headptr)));

   *headptr = NULL;
}


/* Returns a pointer to a copy of event passed as a parameter.  */

event *event_copy (event *orig)
{
   event *new = getfromextraq();
   memmove((char *)new, (char *)orig, DISKSIM_EVENT_SIZE);
/* bcopy ((char *)orig, (char *)new, DISKSIM_EVENT_SIZE); */
   return((event *) new);
}


/*** Functions to manipulate intq, the queue of scheduled events ***/

/* Prints the intq to the output file, presumably for debug.  */


static void disksim_dumpintq ()
{
   event *tmp;
   int i = 0;

   tmp = disksim->intq;
   while (tmp != NULL) {
      i++;
      fprintf (outputfile, "time %f, type %d\n", 
	       tmp->time, tmp->type);
      tmp = tmp->next;
   }
}



/* Add an event to the intq.  The "time" field indicates when the event is */
/* scheduled to occur, and the intq is maintained in ascending time order. */

/* make this a binheap or something ... avoid walking the list */

INLINE void addtointq (event *newint)
{
  /* WARNING -- HORRIBLE HACK BELOW
   * In order to make the Memulator run (which sometimes has events arrive
   * "late") I've (JLG) commented out the following snippet.  A better
   * solution should be arrived at...
   */
#if 0
   if ((temp->time + DISKSIM_TIME_THRESHOLD) < simtime) {
      fprintf(stderr, "Attempting to addtointq an event whose time has passed\n");
      fprintf(stderr, "simtime %f, curr->time %f, type = %d\n", simtime, temp->time, temp->type);
      exit(1);
   }
#endif


   switch(disksim->trace_mode) {
   case DISKSIM_MASTER:
     if(write(disksim->tracepipes[1], (char *)newint, sizeof(event)) <= 0)
       {
	 //	 printf("addtointq() -- master write fd = %d\n", disksim->tracepipes[1]);
	 //	 perror("addtointq() -- master write");
       }
     break;

   case DISKSIM_SLAVE:
     {
       event tmpevt;
       double timediff;

       //       printf("addtointq() -- slave read\n");

       ddbg_assert(read(disksim->tracepipes[0], (char *)&tmpevt, sizeof(event)) > 0);

       timediff = fabs(tmpevt.time - newint->time);
       
/*         printf("remote event: %d %f, local event: %d %f\n", */
/*  	      tmpevt.type,  */
/*  	      tmpevt.time, */
/*  	      newint->type,  */
/*  	      newint->time); */

       ddbg_assert(tmpevt.type == newint->type);
       //       ddbg_assert(timediff <= 0.001);

       newint->time = tmpevt.time;
       if(timediff > 0.000001) {
	 printf("*** SLAVE: addtointq() timediff = %f\n", timediff);
       }
       fflush(stdout);
     }
     break;

   case DISKSIM_NONE:
   default:
     break;
   }


   if (disksim->intq == NULL) {
      disksim->intq = newint;
      newint->next = NULL;
      newint->prev = NULL;
   } 
   else if (newint->time < disksim->intq->time) {
      newint->next = disksim->intq;
      disksim->intq->prev = newint;
      disksim->intq = newint;
      newint->prev = NULL;
   } else {
      event *run = disksim->intq;
      assert(run->next != run);
      while (run->next != NULL) {
         if (newint->time < run->next->time) {
            break;
         }
         run = run->next;
      }
      newint->next = run->next;
      run->next = newint;
      newint->prev = run;
      if (newint->next != NULL) {
         newint->next->prev = newint;
      }
   }
}


/* Retrieves the next scheduled event from the head of the intq. */

INLINE static event * getfromintq ()
{
   event *temp = NULL;

   if (disksim->intq == NULL) {
      return(NULL);
   }
   temp = disksim->intq;
   disksim->intq = disksim->intq->next;
   if (disksim->intq != NULL) {
      disksim->intq->prev = NULL;
   }

   temp->next = NULL;
   temp->prev = NULL;
   return(temp);
}


/* Removes a given event from the intq, thus descheduling it.  Returns */
/* TRUE if the event was found, FALSE if it was not.                   */

INLINE int removefromintq (event *curr)
{
   event *tmp;

   tmp = disksim->intq;
   while (tmp != NULL) {
      if (tmp == curr) {
         break;
      }
      tmp = tmp->next;
   }
   if (tmp == NULL) {
      return(FALSE);
   }
   if (curr->next != NULL) {
      curr->next->prev = curr->prev;
   }
   if (curr->prev == NULL) {
      disksim->intq = curr->next;
   } else {
      curr->prev->next = curr->next;
   }

   curr->next = NULL;
   curr->prev = NULL;
   return(TRUE);
}


/*** Functions to initialize the system and print statistics ***/

void scanparam_int (char *parline, char *parname, int *parptr,
                    int parchecks, int parminval, int parmaxval)
{
   if (sscanf(parline, "%d", parptr) != 1) {
      fprintf(stderr, "Error reading '%s'\n", parname);
      exit(1);
   }
   if (((parchecks & 1) && (*parptr < parminval)) || ((parchecks & 2) && (*parptr > parmaxval))) {
      fprintf(stderr, "Invalid value for '%s': %d\n", parname, *parptr);
      exit(1);
   }
}


void getparam_int (FILE *parfile, char *parname, int *parptr,
                   int parchecks, int parminval, int parmaxval)
{
   char line[201];

   sprintf(line, "%s: %s", parname, "%d");
   if (fscanf(parfile, line, parptr) != 1) {
      fprintf(stderr, "Error reading '%s'\n", parname);
      exit(1);
   }
   fgets(line, 200, parfile); /* this allows comments, kinda ugly hack */
   if (((parchecks & 1) && (*parptr < parminval)) || ((parchecks & 2) && (*parptr > parmaxval))) {
      fprintf(stderr, "Invalid value for '%s': %d\n", parname, *parptr);
      exit(1);
   }
   fprintf (outputfile, "%s: %d\n", parname, *parptr);
}


void getparam_double (FILE *parfile, char *parname, double *parptr,
                      int parchecks, double parminval, double parmaxval)
{
   char line[201];

   sprintf(line, "%s: %s", parname, "%lf");
   if (fscanf(parfile, line, parptr) != 1) {
      fprintf(stderr, "Error reading '%s'\n", parname);
      assert(0);
   }
   fgets(line, 200, parfile);
   if (((parchecks & 1) && (*parptr < parminval)) || ((parchecks & 2) && (*parptr > parmaxval)) || ((parchecks & 4) && (*parptr <= parminval))) {
      fprintf(stderr, "Invalid value for '%s': %f\n", parname, *parptr);
      assert(0);
   }
   fprintf (outputfile, "%s: %f\n", parname, *parptr);
}


void getparam_bool (FILE *parfile, char *parname, int *parptr)
{
   char line[201];

   sprintf(line, "%s %s", parname, "%d");
   if (fscanf(parfile, line, parptr) != 1) {
      fprintf(stderr, "Error reading '%s'\n", parname);
      exit(1);
   }
   fgets(line, 200, parfile);
   if ((*parptr != TRUE) && (*parptr != FALSE)) {
      fprintf(stderr, "Invalid value for '%s': %d\n", parname, *parptr);
      exit(1);
   }
   fprintf (outputfile, "%s %d\n", parname, *parptr);
}


void resetstats ()
{
   if (disksim->external_control | disksim->synthgen | disksim->iotrace) {
      io_resetstats();
   }
   if (disksim->synthgen) {
      pf_resetstats();
   }
}


static void stat_warmup_done (timer_event *timer)
{
   warmuptime = simtime;
   resetstats();
   addtoextraq((event *)timer);
}




int disksim_global_loadparams(struct lp_block *b)
{

  // #include "modules/disksim_global_param.c"
  lp_loadparams(0, b, &disksim_global_mod);

  //  printf("*** warning: seed hack -- using 1207981\n");
  //  DISKSIM_srand48(1207981);
  //  disksim->seedval = 1207981;
  return 1;
}








void disksim_printstats2()
{
   fprintf (outputfile, "\nSIMULATION STATISTICS\n");
   fprintf (outputfile, "---------------------\n\n");
   fprintf (outputfile, "Total time of run:       %f\n\n", simtime);
   fprintf (outputfile, "Warm-up time:            %f\n\n", warmuptime);

   if (disksim->synthgen) {
      pf_printstats();
   }
   if (disksim->external_control | disksim->synthgen | disksim->iotrace) {
      io_printstats();
   }
}


static void setcallbacks ()
{
   disksim->issuefunc_ctlrsmart = NULL;
   disksim->queuefind_ctlrsmart = NULL;
   disksim->wakeupfunc_ctlrsmart = NULL;
   disksim->donefunc_ctlrsmart_read = NULL;
   disksim->donefunc_ctlrsmart_write = NULL;
   disksim->donefunc_cachemem_empty = NULL;
   disksim->donefunc_cachedev_empty = NULL;
   disksim->idlework_cachemem = NULL;
   disksim->idlework_cachedev = NULL;
   disksim->concatok_cachemem = NULL;
   disksim->enablement_disk = NULL;
   disksim->timerfunc_disksim = NULL;
   disksim->timerfunc_ioqueue = NULL;
   disksim->timerfunc_cachemem = NULL;
   disksim->timerfunc_cachedev = NULL;

   disksim->timerfunc_disksim = stat_warmup_done;
   disksim->external_io_done_notify = NULL;

   io_setcallbacks();
   pf_setcallbacks();
}


static void initialize ()
{
   int val = (disksim->synthgen) ? 0 : 1;

   iotrace_initialize_file (disksim->iotracefile, disksim->traceformat, PRINT_TRACEFILE_HEADER);
   while (disksim->intq) {
      addtoextraq(getfromintq());
   }
   if (disksim->external_control | disksim->synthgen | disksim->iotrace) {
      io_initialize(val);
   }
   if (disksim->synthgen) {
      pf_initialize(disksim->seedval);
   } else {
      DISKSIM_srand48(disksim->seedval);
   }
   simtime = 0.0;
}


void disksim_cleanstats ()
{
   if (disksim->external_control | disksim->synthgen | disksim->iotrace) {
      io_cleanstats();
   }
   if (disksim->synthgen) {
      pf_cleanstats();
   }
}


void disksim_set_external_io_done_notify (disksim_iodone_notify_t fn)
{
   disksim->external_io_done_notify = fn;
}


event *io_done_notify (ioreq_event *curr)
{
   if (disksim->synthgen) {
      return pf_io_done_notify(curr, 0);
   }
   if (disksim->external_control) {
      disksim->external_io_done_notify (curr, disksim->notify_ctx);
   }
   return(NULL);
}


INLINE static event * 
getnextevent(void)
{
   event *curr = getfromintq();
   event *temp;

   if (curr) {
      simtime = curr->time;

      if (curr->type == NULL_EVENT) {
	if ((disksim->iotrace) && io_using_external_event(curr)) {
	  temp = io_get_next_external_event(disksim->iotracefile);
	  if (!temp) {
	  #ifndef SSDSIME_STOP_TIME
            disksim->stop_sim = TRUE;
	  #endif
	  }
	  else { 
	    // OK
	  }
	} 
	else {
	  fprintf(stderr, "NULL_EVENT not linked to any external source\n");
	  exit(1);
	}

	if (temp) {
	  temp->type = NULL_EVENT;
	  addtointq(temp);
	}

      } // curr->type == NULL_EVENT
   }
   else {
      fprintf (outputfile, "Returning NULL from getnextevent\n");
      fflush (outputfile);
   }

   return curr;
}


void disksim_checkpoint (char *checkpointfilename)
{
   FILE *checkpointfile;

//printf ("disksim_checkpoint: simtime %f, totalreqs %d\n", simtime, disksim->totalreqs);
   if (disksim->checkpoint_disable) {
      fprintf (outputfile, "Checkpoint at simtime %f skipped because checkpointing is disabled\n", simtime);
      return;
   }

   if ((checkpointfile = fopen(checkpointfilename, "w")) == NULL) {
      fprintf (outputfile, "Checkpoint at simtime %f skipped because checkpointfile cannot be opened for write access\n", simtime);
      return;
   }

   if (disksim->iotracefile) {
      if (strcmp (disksim->iotracefilename, "stdin") == 0) {
         fprintf (outputfile, "Checkpoint at simtime %f skipped because iotrace comes from stdin\n", simtime);
         return;
      }
      fflush (disksim->iotracefile);
      fgetpos (disksim->iotracefile, &disksim->iotracefileposition);
   }

   if (outios) {
      fflush (outios);
      fgetpos (outios, &disksim->outiosfileposition);
   }

   if (outputfile) {
      fflush (outputfile);
      fgetpos (outputfile, &disksim->outputfileposition);
   }

   rewind (checkpointfile);
   fwrite (disksim, disksim->totallength, 1, checkpointfile);
   fflush (checkpointfile);
   fclose (checkpointfile);
}


void disksim_simstop ()
{
   disksim->stop_sim = TRUE;
}


void disksim_register_checkpoint (double attime)
{
   event *checkpoint = getfromextraq ();
   checkpoint->type = CHECKPOINT;
   checkpoint->time = attime;
   addtointq(checkpoint);
}


static void prime_simulation ()
{
   event *curr;

   if (disksim->warmup_event) {
      addtointq((event *)disksim->warmup_event);
      disksim->warmup_event = NULL;
   }
   if (disksim->checkpoint_interval > 0.0) {
      disksim_register_checkpoint (disksim->checkpoint_interval);
   }
   if (disksim->iotrace) {
      if ((curr = io_get_next_external_event(disksim->iotracefile)) == NULL) {
         disksim_cleanstats();
         return;
      }
      if ((disksim->traceformat != VALIDATE) && (disksim->closedios == 0)) {
         curr->type = NULL_EVENT;
      } else if (disksim->closedios) {
         int iocnt;
         io_using_external_event(curr);
         curr->time = simtime + disksim->closedthinktime;
         for (iocnt=1; iocnt < disksim->closedios; iocnt++) {
            curr->next = io_get_next_external_event(disksim->iotracefile);
            if (curr->next) {
               io_using_external_event(curr->next);
               curr->time = simtime + disksim->closedthinktime;
               addtointq(curr->next);
            }
         }
      }

      // XXX yuck ... special case for 1st req
      if(disksim->traceformat == VALIDATE) {
	ioreq_event *tmp = (ioreq_event *)curr;
	disksim_exectrace("Request issue: simtime %f, devno %d, blkno %d, time %f\n", 
			  simtime, tmp->devno, tmp->blkno, tmp->time);
      }

      addtointq(curr);
   }
}


void disksim_simulate_event (int num)//*jian vsd's request schedule (1)

{
  event *curr;

  if ((curr = getnextevent()) == NULL) {
    disksim_simstop ();
  } 
  else {
   
    switch(disksim->trace_mode) {
    case DISKSIM_NONE:
    case DISKSIM_MASTER:
/*        fprintf(outputfile, "*** DEBUG TRACE\t%f\t%d\t%d\n", */
/*  	      simtime, curr->type, num); */
/*        fflush(outputfile); */
      break;

    case DISKSIM_SLAVE:
      break;
    }
    

    simtime = curr->time;
#ifdef SSDSIME_STOP_TIME
  if (simtime >= SSDSIME_STOP_TIME) {
    disksim_simstop ();
  } 
#endif

if(simtime < 0)
{
	fprintf(stderr,"j_debug even->type = %d (simtime = %f)\n",curr->type,simtime);
	/*user_time_state *ptr_k = user_time->next;
        while(ptr_k != user_time)
        {
               fprintf(stderr,"j_debug 2 (user_id = %d) ptr->curr_quota=%f ptr->waiting_request_cnt=%d \n"\
               ,ptr_k->user_id,ptr_k->curr_quota,ptr_k->waiting_request_cnt);
               ptr_k = ptr_k->next;
        }
	fprintf(stderr,"j_debug runing_requests_cnt_in_ssd=%d\n",runing_requests_cnt_in_ssd);
	fprintf(stderr,"j_debug total_waiting_requests_cnt=%d\n",total_waiting_requests_cnt);
	//fprintf(outputfile,"j_debug total_queue_requests_cnt=%d\n",total_waiting_requests_cnt + runing_requests_cnt_in_ssd);
	//fprintf(outputfile,"j_debug max_queue_requests_cnt=%d\n",1000000);//***.parv 194 Max queue length =  1000,
	fprintf(stderr,"j_debug total_finished_requests_cnt=%d\n\n\n",total_finished_requests_cnt);*/

}
   
    if (curr->type == INTR_EVENT) 
    {
      intr_acknowledge (curr);
    } 
    else if ((curr->type >= IO_MIN_EVENT) && (curr->type <= IO_MAX_EVENT)) 
    {
	//*jian_m
	#ifdef ACCESS_USER_BANDWIDTH
	
	if(curr->type==106)//*jian 106 == (ssd) DEVICE_OVERHEAD_COMPLETE (request first coming)
        {
		//*jian request coming put in per user queue && global queue		
        }
	//else
	#endif
	//*jian_m
      	io_internal_event ((ioreq_event *)curr);
    } 
    else if ((curr->type >= PF_MIN_EVENT) && (curr->type <= PF_MAX_EVENT)) 
    {
      pf_internal_event(curr);
    }
    else if (curr->type == TIMER_EXPIRED) 
    {
      timer_event *timeout = (timer_event *) curr;
      (*timeout->func) (timeout);
    } 
    else if ((curr->type >= MEMS_MIN_EVENT) 
	     && (curr->type <= MEMS_MAX_EVENT)) 
    {
      io_internal_event ((ioreq_event *)curr);
    }
    /* SSD: These are ssd specific events */
    else if ((curr->type >= SSD_MIN_EVENT)
         && (curr->type <= SSD_MAX_EVENT))
    {
      io_internal_event ((ioreq_event *)curr);
    }
    else if (curr->type == CHECKPOINT)
    {
      if (disksim->checkpoint_interval) {
	disksim_register_checkpoint(simtime + disksim->checkpoint_interval);
      }
      disksim_checkpoint (disksim->checkpointfilename);
    } 
    else if (curr->type == STOP_SIM) 
    {
      disksim_simstop ();
    } 
    else if (curr->type == EXIT_DISKSIM) 
    {
      exit (0);
    }
    #ifdef PRINT_THROUGHPUT
    else if (curr->type == PRINT_PER_USER_THOUPUT_EVENT)//*jian_m choose request put in ssd 253
    {
	user_time_state *ptr = user_time->next;
	if(user_time->blocks_per_sec > 0)
	{
		while(ptr != user_time)
        	{
			double temp_troughput = (double)ptr->blocks_per_sec/2048;
			//fprintf(outputfile,"j_debug time= %f (user_id = %d) blocks = %u (Throughput %f MB/s)(blocks cnt) ios = %u",\
			//	 simtime, ptr->user_id, ptr->blocks_per_sec,temp_troughput, ptr->ios_per_sec);
			//fprintf(outputfile,"Time= %f (user_id = %d) (Throughput %f MB/s) (GC_time %f ms/s) (Avg_response_time %f (ms/req)/s) (Avg_bcount %f )",\
				simtime, ptr->user_id, temp_troughput, ptr->total_clean_time_per_sec, ptr->response_time_per_sec/ptr->ios_per_sec,(double)ptr->blocks_per_sec/ptr->ios_per_sec);

			fprintf(outputfile,"Time= %f uid= %d Throught= %f GC_time= %f Avg_resp.= %f page_copy= %d GC_issue= %d",simtime, ptr->user_id, temp_troughput, ptr->total_clean_time_per_sec, ptr->response_time_per_sec/ptr->ios_per_sec,ptr->gc_live_page_copy_per_sec,ptr->gc_issues_per_sec);

			#ifdef ACCESS_USER_BANDWIDTH
				if(ptr->curr_quota>=0)
				fprintf(outputfile," spare time= %f\n",ptr->curr_quota);
				else
				fprintf(outputfile," spare time= %f\n",0.0);
			#endif
			#ifndef ACCESS_USER_BANDWIDTH
                                fprintf(outputfile," \n");
                        #endif
			//fprintf(stderr,"j_debug User(%d)- using_block_cnt %d using_spare_block_cnt %d simtime(%f / %f)\n"\
			//,ptr->user_id,ptr->using_block_cnt,ptr->using_spare_block_cnt,simtime,SSDSIME_STOP_TIME);//*jian check
			fprintf(stderr,"j_debug User(%d)- using_pages_cnt %d using_pages_per_sec %ld simtime(%f)\n"\
			,ptr->user_id,ptr->using_pages_cnt,ptr->using_pages_per_sec,simtime);//*jian check
			fflush(outputfile);
			ptr->blocks_per_sec = 0;
			ptr->total_clean_time_per_sec = 0;
			ptr->response_time_per_sec = 0;
			ptr->ios_per_sec = 0;
			// hu moddify for per-sec output
			ptr->gc_live_page_copy_per_sec = 0;//hu
			ptr->gc_issues_per_sec = 0; //hu
			ptr->using_pages_per_sec = 0; //hu
			ptr->live_page_per_sec = 0; //hu
			// end of hu modify
			ptr = ptr->next;
		}
	}

	event *new = getfromextraq();//add a even's head which about "reallocating to per user's time quota"
        new->time = simtime + 1000.0;
        new->type = PRINT_PER_USER_THOUPUT_EVENT;//*jian 253
        addtointq(new);
        addtoextraq((event *)curr);
	#ifdef ACCESS_USER_BANDWIDTH
    	#ifndef USE_SPARE_BAMDWIDTH
					new = getfromextraq();//add a even's head which about "reallocating to per user's time quota"
	                                new->time = simtime;
        	                        new->type = GIVING_PER_USER_QUOTA_EVENT;//*jian 255
                	                addtointq(new);
    	#endif
	#endif

        disksim_simulate_event (num);
    }
    #endif
    #ifdef ACCESS_USER_BANDWIDTH
    /*else if (curr->type == CHOOSE_USER_TO_RUN_EVENT)//*jian_m choose request put in ssd 254
    {
    }
    else if (curr->type == GIVING_PER_USER_QUOTA_EVENT)//*jian_m reallocating per user time quota 255
    {
    }*/
    #endif
    else 
    {
      fprintf(stderr, "Unrecognized event in simulate: %d\n", curr->type);
      exit(1);
    }


#ifdef FDEBUG
    fprintf (outputfile, "Event handled, going for next\n");
    fflush (outputfile);
#endif
  }

  
}


static void disksim_setup_outputfile (char *filename, char *mode)
{
   if (strcmp(filename, "stdout") == 0) {
      outputfile = stdout;
   } 
   else {
     if ((outputfile = fopen(filename,mode)) == NULL) {
       fprintf(stderr, "Outfile %s cannot be opened for write access\n", filename);
       exit(1);
     }
   }
   
   if (strlen(filename) <= 255) {
      strcpy (disksim->outputfilename, filename);
   } 
   else {
     fprintf (stderr, "Name of output file is too long (>255 bytes); checkpointing disabled\n");
     disksim->checkpoint_disable = 1;
   }

   setvbuf(outputfile, 0, _IONBF, 0); 
}


static void disksim_setup_iotracefile (char *filename)
{
   if (strcmp(filename, "0") != 0) {
      assert (disksim->external_control == 0);
      disksim->iotrace = 1;
      if (strcmp(filename, "stdin") == 0) {
	 disksim->iotracefile = stdin;
      } else {
	 if ((disksim->iotracefile = fopen(filename,"rb")) == NULL) {
	    fprintf(stderr, "Tracefile %s cannot be opened for read access\n", filename);
	    exit(1);
	 }
      }
   } else {
      disksim->iotracefile = NULL;
   }

   if (strlen(filename) <= 255) {
      strcpy (disksim->iotracefilename, filename);
   } else {
      fprintf (stderr, "Name of iotrace file is too long (>255 bytes); checkpointing disabled\n");
      disksim->checkpoint_disable = 1;
   }
}


static void setEndian(void) {
  int n = 0x11223344;
  char *c = (char *)&n;

  if(c[0] == 0x11) {
    disksim->endian = _BIG_ENDIAN;
  }
  else {
    disksim->endian = _LITTLE_ENDIAN;
  }
 
}

void disksim_setup_disksim (int argc, char **argv)
{
  parfile=argv[1];//*jian_m get parv file name
  setEndian();
  
  StaticAssert (sizeof(intchar) == 4);
  if (argc < 6) {
    fprintf(stderr,"Usage: %s paramfile outfile format iotrace synthgen?\n", argv[0]);
    exit(1);
  }

#ifdef FDEBUG
  fprintf (stderr, "%s %s %s %s %s %s\n", argv[0], argv[1], argv[2], argv[3],
	   argv[4], argv[5]);
#endif

  if ((argc - 6) % 3) {
    fprintf(stderr, "Parameter file overrides must be 3-tuples\n");
    exit(1);
   } 
   
  disksim_setup_outputfile (argv[2], "w");
  fprintf (outputfile, "\n*** Output file name: %s\n", argv[2]); 
  fflush (outputfile); 
   
  if(strcmp (argv[3], "external") == 0) {
    disksim->external_control = 1;
  } 
  else {
    iotrace_set_format(argv[3]);
  }

  fprintf (outputfile, "*** Input trace format: %s\n", argv[3]); 
  fflush (outputfile); 

  disksim_setup_iotracefile (argv[4]);
  fprintf (outputfile, "*** I/O trace used: %s\n", argv[4]); 
  fflush (outputfile); 

  if(strcmp(argv[5], "0") != 0) {
    disksim->synthgen = 1;
    disksim->iotrace = 0;
  }

  fprintf(outputfile, "*** Synthgen to be used?: %d\n\n", 
	  disksim->synthgen); 

  fflush(outputfile); 

  disksim->overrides = argv + 6;
  disksim->overrides_len = argc - 6;

   // asserts go to stderr by default
  ddbg_assert_setfile(stderr);

  
  disksim->tracepipes[0] = 8; 
  disksim->tracepipes[1] = 9; 
  
 /*   disksim->tracepipes[0] = dup(8); */
/*    disksim->tracepipes[1] = dup(9); */
/*    close(8); */
/*    close(9); */

/*    if(!strcmp(argv[6], "master")) { */
/*      printf("*** Running in MASTER mode\n"); */
/*      disksim->trace_mode = DISKSIM_MASTER; */
/*      close(disksim->tracepipes[0]); */
/*    } */
/*    else if(!strcmp(argv[6], "slave")) { */
/*      printf("*** Running in SLAVE mode\n"); */
/*      disksim->trace_mode = DISKSIM_SLAVE; */
/*      close(disksim->tracepipes[1]); */
/*    } */
/*    else { */
  //    printf("*** not running in master-slave mode\n");
    disksim->trace_mode = DISKSIM_NONE;
/*    } */
  
  if(disksim_loadparams(argv[1], disksim->synthgen)) {
    fprintf(stderr, "*** error: FATAL: failed to load disksim parameter file.\n");
    exit(1);
  }
  else {
    fprintf(outputfile, "loadparams complete\n");
    fflush(outputfile);
    /*       config_release_typetbl(); */
  }
  if(!disksim->iosim_info) {
    /* if the user didn't provide an iosim block */
    iosim_initialize_iosim_info ();
  }



  initialize();
  fprintf(outputfile, "Initialization complete\n");
  fflush(outputfile);
  prime_simulation();


  // XXX abstract this
  fclose(statdeffile);
  statdeffile = NULL;
}


void disksim_restore_from_checkpoint (char *filename)
{
#ifdef SUPPORT_CHECKPOINTS
   int fd;
   void *startaddr;
   int totallength;
   int ret;

   fd = open (filename, O_RDWR);
   assert (fd >= 0);
   ret = read (fd, &startaddr, sizeof (void *));
   assert (ret == sizeof(void *));
   ret = read (fd, &totallength, sizeof (int));
   assert (ret == sizeof(int));
   disksim = (disksim_t *) mmap (startaddr, totallength, (PROT_READ|PROT_WRITE), (MAP_PRIVATE), fd, 0);
   //printf ("mmap at %p, ret %d, len %d, addr %p\n", disksim, ret, totallength, startaddr);
   //perror("");
   assert (disksim == startaddr);

   disksim_setup_outputfile (disksim->outputfilename, "r+");
   if (outputfile != NULL) {
      ret = fsetpos (outputfile, &disksim->outputfileposition);
      assert (ret >= 0);
   }
   disksim_setup_iotracefile (disksim->iotracefilename);
   if (disksim->iotracefile != NULL) {
      ret = fsetpos (disksim->iotracefile, &disksim->iotracefileposition);
      assert (ret >= 0);
   }
   if ((strcmp(disksim->outiosfilename, "0") != 0) && (strcmp(disksim->outiosfilename, "null") != 0)) {
      if ((outios = fopen(disksim->outiosfilename, "r+")) == NULL) {
         fprintf(stderr, "Outios %s cannot be opened for write access\n", disksim->outiosfilename);
         exit(1);
      }
      ret = fsetpos (outios, &disksim->outiosfileposition);
      assert (ret >= 0);
   }
#else
   assert ("Checkpoint/restore not supported on this platform" == 0);
#endif
   setcallbacks();
}


void disksim_run_simulation ()
{
  int event_count = 0;
  DISKSIM_srand48(1000003);
  while (disksim->stop_sim == FALSE) {
    disksim_simulate_event(event_count);
    //    printf("disksim_run_simulation: event %d\n", event_count);
    event_count++;
  }
  // printf("disksim_run_simulation(): simulated %d events\n", event_count);
}



void disksim_cleanup(void) 
{
  int i;
  
  if (outputfile) 
  {
    fclose (outputfile);
  }
  
  
  if (disksim->iotracefile) 
  {
    fclose(disksim->iotracefile);
  }
  
  
  iodriver_cleanup();
  
  if(outios) 
  {
    fclose(outios);
    outios = NULL;
  }
}

void disksim_printstats(void) {
   fprintf(outputfile, "Simulation complete\n");
   fflush(outputfile);

   disksim_cleanstats();
   disksim_printstats2();
}

void disksim_cleanup_and_printstats ()
{
  disksim_printstats();
  disksim_cleanup();
}


int 
disksim_initialize_disksim_structure (struct disksim *disksim)
{

   //   disksim->startaddr = addr;
   //   disksim->totallength = len;
   //   disksim->curroffset = sizeof(disksim_t);

   disksim->closedthinktime = 0.0;
   warmuptime = 0.0;	/* gets remapped to disksim->warmuptime */
   simtime = 0.0;	/* gets remapped to disksim->warmuptime */
   disksim->lastphystime = 0.0;
   disksim->checkpoint_interval = 0.0;

   return 0;
}


void
disksim_exectrace(char *fmt, ...) {

  if(disksim->exectrace) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(disksim->exectrace, fmt, ap);
    va_end(ap);
  }
}

