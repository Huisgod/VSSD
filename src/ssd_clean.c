// DiskSim SSD support
// ©2008 Microsoft Corporation. All Rights Reserved

#include "ssd.h"
#include "ssd_clean.h"
#include "ssd_utils.h"
#include "modules/ssdmodel_ssd_param.h"

/*
 * return true if two blocks belong to the same plane.
 */
static int ssd_same_plane_blocks
(ssd_t *s, ssd_element_metadata *metadata, int from_blk, int to_blk)
{
    return (metadata->block_usage[from_blk].plane_num == metadata->block_usage[to_blk].plane_num);
}

/*
 * we clean a block only after it is fully used.
 */
int ssd_can_clean_block(ssd_t *s, ssd_element_metadata *metadata, int blk)
{
    int bitpos = ssd_block_to_bitpos(s, blk);
    return ((ssd_bit_on(metadata->free_blocks, bitpos)) && (metadata->block_usage[blk].state == SSD_BLOCK_SEALED));
}

/*
 * calculates the cost of reading and writing a block of data across planes.
 */
static double ssd_crossover_cost
(ssd_t *s, ssd_element_metadata *metadata, int from_blk, int to_blk)
{
    if (ssd_same_plane_blocks(s, metadata, from_blk, to_blk)) {
        return 0;
    } else {
        double xfer_cost;

        // we need to read and write back across the pins
        xfer_cost = ssd_data_transfer_cost(s, s->params.page_size);
        return (2 * xfer_cost);
    }
}

/*
 * writes a page to the current active page. if there is no active page,
 * allocate one and then move.
 */
static double ssd_move_page(int lpn, int from_blk, int plane_num, int elem_num, ssd_t *s, int user_id, int pp_index)
{
    double cost = 0;
    ssd_element_metadata *metadata = &s->elements[elem_num].metadata;
    switch(s->params.copy_back) {
        case SSD_COPY_BACK_DISABLE:
            if (ssd_last_page_in_block(metadata->active_page[user_id], s)) {
                _ssd_alloc_active_block(-1, elem_num, s,user_id);
            }
            break;

        case SSD_COPY_BACK_ENABLE:
	    #ifdef GC_SELF_BLOCKS
	    if(metadata->plane_meta[plane_num].active_page[user_id] != metadata->active_page[user_id])
	    {
		fprintf(stderr,"plane = %d user_id = %d\n",plane_num,user_id);
		fprintf(stderr,"metadata->plane_meta[plane_num].active_page[user_id] = %d\n",metadata->plane_meta[plane_num].active_page[user_id]);
		fprintf(stderr,"metadata->active_page[user_id]=%d\n",metadata->active_page[user_id]);
            	ASSERT(metadata->plane_meta[plane_num].active_page[user_id] == metadata->active_page[user_id]);	//*jian modify 
	    }
	    #endif
	    #ifndef GC_SELF_BLOCKS
		metadata->active_page[user_id] = metadata->plane_meta[plane_num].active_page[user_id];
	    #endif
            if (ssd_last_page_in_block(metadata->active_page[user_id], s)) {
                _ssd_alloc_active_block(plane_num, elem_num, s, user_id);
            }
            break;

        default:
            fprintf(stderr, "Error: invalid copy back policy %d\n",
                s->params.copy_back);
            exit(1);
    }

#ifndef CURREN_BLOCK_ENABLE
#ifdef DEBUG_VSSD
    cost += _ssd_write_page_osr(s, metadata, lpn,metadata->block_usage[from_blk].page_user_id[pp_index]);
#endif
#endif
#ifdef CURREN_BLOCK_ENABLE
    cost += _ssd_write_page_osr(s, metadata, lpn, user_id);
#endif

    //*************************jian add***********************
    if(metadata->plane_meta[plane_num].free_pages[user_id] <= 0)
    {
        metadata->plane_meta[plane_num].free_pages[0] -= SSD_DATA_PAGES_PER_BLOCK(s);
        metadata->plane_meta[plane_num].free_pages[user_id] += SSD_DATA_PAGES_PER_BLOCK(s);
        metadata->plane_meta[plane_num].using_nonallocating_pages[user_id] += SSD_DATA_PAGES_PER_BLOCK(s);
	ASSERT(metadata->plane_meta[plane_num].free_pages[0] >= 0);
    }
        metadata->plane_meta[plane_num].free_pages[user_id] --;
    //*************************jian add***********************

    return cost;
}

/*
 * reads the data out of a page and writes it back the active page.
 */
static double ssd_clean_one_page
(int lp_num, int pp_index, int blk, int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    double cost = 0;
    double xfer_cost = 0;
    int user_id = metadata->block_usage[blk].user_id;

    cost += s->params.page_read_latency;


    cost += ssd_move_page(lp_num, blk, plane_num, elem_num, s,user_id,pp_index);

    // if the write is within the same plane, then the data need
    // not cross the pins. but if not, add the cost of transferring
    // the bytes across the pins
    xfer_cost = ssd_crossover_cost(s, metadata, blk, SSD_PAGE_TO_BLOCK(metadata->active_page[user_id], s));

    cost += xfer_cost;

    ssd_assert_valid_pages(plane_num, metadata, s);
    ASSERT(metadata->block_usage[blk].page[pp_index] == -1);

    // stat -- we move 'valid_pages' out of this block
    s->elements[elem_num].stat.pages_moved ++;
    s->elements[elem_num].stat.tot_xfer_cost += xfer_cost;

    return cost;
}

/*
 * update the remaining life time and time of last erasure
 * for this block.
 * we did some operations since the last update to
 * simtime variable and the time took for these operations are
 * in the 'cost' variable. so the time of last erasure is
 * cost + the simtime
 */
void ssd_update_block_lifetime(double time, int blk, ssd_element_metadata *metadata)
{
    metadata->block_usage[blk].rem_lifetime --;
    metadata->block_usage[blk].time_of_last_erasure = time;

    if (metadata->block_usage[blk].rem_lifetime < 0) {
        fprintf(stderr, "Error: Negative lifetime %d (block is being erased after it's dead)\n",
            metadata->block_usage[blk].rem_lifetime);
        ASSERT(0);
        exit(1);
    }
}

/*
 * updates the status of erased blocks
 */
void ssd_update_free_block_status(int blk, int plane_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int bitpos;
    int user_id = metadata->block_usage[blk].user_id;
    user_time_state *ptr;

    // clear the bit corresponding to this block in the
    // free blocks list for future use
    bitpos = ssd_block_to_bitpos(s, blk);
    ssd_clear_bit(metadata->free_blocks, bitpos);
    metadata->block_usage[blk].state = SSD_BLOCK_CLEAN;
    metadata->block_usage[blk].bsn[metadata->user_id] = 0;
    metadata->tot_free_blocks ++;
    metadata->plane_meta[plane_num].free_blocks ++;
    ssd_assert_free_blocks(s, metadata);

    //*jian add
    int want_free_pages = SSD_DATA_PAGES_PER_BLOCK(s);
    if(metadata->plane_meta[plane_num].using_nonallocating_pages[user_id] >= SSD_DATA_PAGES_PER_BLOCK(s))
    {
	//***********for non-alllcating space************
	metadata->plane_meta[plane_num].using_nonallocating_pages[user_id] -= SSD_DATA_PAGES_PER_BLOCK(s);
	metadata->plane_meta[plane_num].free_pages[0] += SSD_DATA_PAGES_PER_BLOCK(s);
    }
    else if(metadata->plane_meta[plane_num].using_nonallocating_pages[user_id] > 0)
    {
	//***********for non-alllcating space and user_space************
	ASSERT(0);
	/*metadata->free_pages[0] += metadata->using_nonallocating_pages[user_id];
	want_free_pages -= metadata->using_nonallocating_pages[user_id];
	metadata->using_nonallocating_pages[user_id] = 0;

	metadata->free_pages[user_id] += want_free_pages;*/
    }
    else
    {
	//***********for user's space**************
	metadata->plane_meta[plane_num].free_pages[user_id] += SSD_DATA_PAGES_PER_BLOCK(s);
    }

    #ifdef CURREN_BLOCK_ENABLE
    #ifdef GC_SELF_BLOCKS
    if(user_id != 0)
    {
	    metadata->user_using_blocks[user_id]--;
	    metadata->plane_meta[plane_num].user_using_blocks[user_id]--;
  

	/*    ptr = find_user(user_id);
	    if( (ptr->using_spare_block_cnt+ptr->using_block_cnt) <= ptr->aloc_block_cnt)
	    {
		ptr->using_spare_block_cnt--;
	    }
	    else if( (ptr->using_spare_block_cnt+ptr->using_block_cnt) > ptr->aloc_block_cnt)
	    {
		user_time->using_block_cnt--;
        	ptr->using_spare_block_cnt--;
	    }
	    else if( ptr->using_block_cnt <= 0 )
		ptr->using_block_cnt--;
	    else
    	{
		fprintf(stderr,"Error !");
		ASSERT(0);
    	}*/
    }
    #endif
    #endif
    metadata->block_usage[blk].user_id = 0;

    // there must be no valid pages in the erased block
    ASSERT(metadata->block_usage[blk].num_valid == 0);
    ssd_assert_valid_pages(plane_num, metadata, s);
}

//#define CAMERA_READY

/*
 * returns one if the block must be rate limited
 * because of its reduced block life else returns 0, if the
 * block can be continued with cleaning.
 * CAMERA READY: adding rate limiting according to the new
 * definition
 */
#ifdef CAMERA_READY
int ssd_rate_limit(int block_life, double avg_lifetime)
{
    double percent_rem = (block_life * 1.0) / avg_lifetime;
    double temp = (percent_rem - (SSD_LIFETIME_THRESHOLD_X-SSD_RATELIMIT_WINDOW)) / (SSD_LIFETIME_THRESHOLD_X - percent_rem);
    double rand_no = DISKSIM_drand48();

    // i can use this block
    if (rand_no < temp) {
        return 0;
    } else {
        //i cannot use this block
        //if (rand_no >= temp)
        return 1;
    }
}
#else
int ssd_rate_limit(int block_life, double avg_lifetime)
{
    double percent_rem = (block_life * 1.0) / avg_lifetime;
    double temp = percent_rem / SSD_LIFETIME_THRESHOLD_X;
    double rand_no = DISKSIM_drand48();

    // i can use this block
    if (rand_no < temp) {
        return 0;
    } else {
        //i cannot use this block
        //if (rand_no >= temp)
        return 1;
    }
}
#endif

/*
 * computes the average lifetime of all the blocks in a plane.
 */
double ssd_compute_avg_lifetime_in_plane(int plane_num, int elem_num, ssd_t *s)
{
    int i;
    int bitpos;
    double tot_lifetime = 0;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    bitpos = plane_num * s->params.blocks_per_plane;
    for (i = bitpos; i < bitpos + (int)s->params.blocks_per_plane; i ++) {
        int block = ssd_bitpos_to_block(i, s);
        ASSERT(metadata->block_usage[block].plane_num == plane_num);
        tot_lifetime += metadata->block_usage[block].rem_lifetime;
    }

    return (tot_lifetime / s->params.blocks_per_plane);
}


/*
 * computes the average lifetime of all the blocks in the ssd element.
 */
double ssd_compute_avg_lifetime_in_element(int elem_num, ssd_t *s)
{
    double tot_lifetime = 0;
    int i;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    for (i = 0; i < s->params.blocks_per_element; i ++) {
        tot_lifetime += metadata->block_usage[i].rem_lifetime;
    }

    return (tot_lifetime / s->params.blocks_per_element);
}

double ssd_compute_avg_lifetime(int plane_num, int elem_num, ssd_t *s)
{
    if (plane_num == -1) {
        return ssd_compute_avg_lifetime_in_element(elem_num, s);
    } else {
        return ssd_compute_avg_lifetime_in_plane(plane_num, elem_num, s);
    }
}

#ifdef CAMERA_READY
// if we care about wear-leveling, then we must rate limit overly cleaned blocks.
// return 1 if it is ok to clean this block.
// CAMERA READY: making changes for the camera ready.
// if the block life is less than lifetime threshold x and within
// a rate limit window, then it is rate limited with probability
// that linearly increases from 0 from 1 as its remaining life time
// drops from (SSD_LIFETIME_THRESHOLD_X * avg_lifetime) to
// (SSD_LIFETIME_THRESHOLD_X - SSD_RATELIMIT_WINDOW) * avg_lifetime.
// if it is below (SSD_LIFETIME_THRESHOLD_X - SSD_RATELIMIT_WINDOW) * avg_lifetime
// then the probability is 0.
int ssd_pick_wear_aware(int blk, int block_life, double avg_lifetime, ssd_t *s)
{
    ASSERT(s->params.cleaning_policy == DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE);

    // see if this block's remaining lifetime is within
    // a certain threshold of the average remaining lifetime
    // of all blocks in this element
    if (block_life < (SSD_LIFETIME_THRESHOLD_X * avg_lifetime)) {
        if ((block_life > (SSD_LIFETIME_THRESHOLD_X - SSD_RATELIMIT_WINDOW) * avg_lifetime)) {
            // we have to rate limit this block as it has exceeded
            // its cleaning limits
            //printf("Rate limiting block %d (block life %d avg life %f\n",
            //  blk, block_life, avg_lifetime);

            if (ssd_rate_limit(block_life, avg_lifetime)) {
                // skip this block and go to the next one
                return 0;
            }
        } else {
            // this block's lifetime is less than (SSD_LIFETIME_THRESHOLD_X - SSD_RATELIMIT_WINDOW)
            // of the average life time.
            // skip this block and go to the next one
            return 0;
        }
    }

    return 1;
}
#else
// if we care about wear-leveling, then we must rate limit overly cleaned blocks.
// return 1 if it is ok to clean this block
int ssd_pick_wear_aware(int blk, int block_life, double avg_lifetime, ssd_t *s)
{
    ASSERT(s->params.cleaning_policy == DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE);

    // see if this block's remaining lifetime is within
    // a certain threshold of the average remaining lifetime
    // of all blocks in this element
    if (block_life < (SSD_LIFETIME_THRESHOLD_X * avg_lifetime)) {

        // we have to rate limit this block as it has exceeded
        // its cleaning limits
        //printf("Rate limiting block %d (block life %d avg life %f\n",
        //  blk, block_life, avg_lifetime);

        if (ssd_rate_limit(block_life, avg_lifetime)) {
            // skip this block and go to the next one
            return 0;
        }
    }

    return 1;
}
#endif

static int _ssd_pick_block_to_clean
(int blk, int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int block_life;
    user_time_state *ptr;
#ifdef ALLOCATING_PLANE
	if(metadata->plane_meta[metadata->block_usage[blk].plane_num].user_id != metadata->user_id)
	return 0;
#endif

    if (plane_num != -1) {
        if (metadata->block_usage[blk].plane_num != plane_num) {
            return 0;
        }
    }

    block_life = metadata->block_usage[blk].rem_lifetime;

    // if the block is already dead, skip it
    if (block_life == 0) {
        return 0;
    }

    // clean only those blocks that are sealed.
    if (!ssd_can_clean_block(s, metadata, blk)) {
        return 0;
    }
#ifdef GC_SELF_BLOCKS
	/*if (metadata->block_usage[blk].user_id > 0)
	{
		if (metadata->block_usage[blk].user_id != metadata->user_id)
    			return 0;
	} */
#endif
    if(metadata->block_usage[blk].user_id ==0)
	return 0;	
	//*jian add GC self block
	/*#ifdef GC_SELF_BLOCKS
	ptr = find_user(metadata->user_id);

	if(ptr->using_spare_block_cnt > 0)
	{
		if (metadata->block_usage[blk].user_id == metadata->user_id)
		return 1;
		else if (metadata->block_usage[blk].user_id == 0)
		return 1;
		else
		return 0;
	}
	else if(ptr->using_block_cnt > 0)
	{
		if (metadata->block_usage[blk].user_id == metadata->user_id)
		return 1;
		else if (metadata->block_usage[blk].user_id == 0)
		return 1;
		else
		return 0;
	}
	#endif*/
	//*jian add GC self block

    return 1;
}

/*
 * migrate data from a cold block to "to_blk"
 */
int ssd_migrate_cold_data(int to_blk, double *mcost, int plane_num, int elem_num, ssd_t *s)
{
    int i;
    int from_blk = -1;
    double oldest_erase_time = simtime;
    double cost = 0;
    int bitpos;
    user_time_state *ptr;

#if SSD_ASSERT_ALL
    int f1;
    int f2;
#endif

    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    // first select the coldest of all blocks.
    // one way to select is to find the one that has the oldest
    // erasure time.
    if (plane_num == -1) {
        for (i = 0; i < s->params.blocks_per_element; i ++) {
            if (metadata->block_usage[i].num_valid > 0) {
                if (metadata->block_usage[i].time_of_last_erasure < oldest_erase_time) {
                    oldest_erase_time = metadata->block_usage[i].time_of_last_erasure;
                    from_blk = i;
                }
            }
        }
    } else {

#if SSD_ASSERT_ALL
        f1 = ssd_free_bits(plane_num, elem_num, metadata, s);
        ASSERT(f1 == metadata->plane_meta[metadata->block_usage[to_blk].plane_num].free_blocks);
#endif

        bitpos = plane_num * s->params.blocks_per_plane;
        for (i = bitpos; i < bitpos + (int)s->params.blocks_per_plane; i ++) {
            int block = ssd_bitpos_to_block(i, s);
            ASSERT(metadata->block_usage[block].plane_num == plane_num);

            if (metadata->block_usage[block].num_valid > 0) {
                if (metadata->block_usage[block].time_of_last_erasure < oldest_erase_time) {
                    oldest_erase_time = metadata->block_usage[block].time_of_last_erasure;
                    from_blk = block;
                }
            }
        }
    }

    ASSERT(from_blk != -1);
    if (plane_num != -1) {
        ASSERT(metadata->block_usage[from_blk].plane_num == metadata->block_usage[to_blk].plane_num);
    }

    // next, clean the block to which we'll transfer the
    // cold data
    cost += _ssd_clean_block_fully(to_blk, metadata->block_usage[to_blk].plane_num, elem_num, metadata, s);

#if SSD_ASSERT_ALL
    if (plane_num != -1) {
        f2 = ssd_free_bits(plane_num, elem_num, metadata, s);
        ASSERT(f2 == metadata->plane_meta[metadata->block_usage[to_blk].plane_num].free_blocks);
    }
#endif

    // then, migrate the cold data to the worn out block.
    // for which, we first read all the valid data
    cost += metadata->block_usage[from_blk].num_valid * s->params.page_read_latency;
    // include the write cost
    cost += metadata->block_usage[from_blk].num_valid * s->params.page_write_latency;
    // if the src and dest blocks are on different planes
    // include the transfer cost also
    cost += ssd_crossover_cost(s, metadata, from_blk, to_blk);

    // the cost of erasing the cold block (represented by from_blk)
    // will be added later ...
#ifdef DEBUG_VSSD
    for (i = 0; i < s->params.pages_per_block; i ++) {
	metadata->block_usage[to_blk].page_user_id[i] = metadata->block_usage[from_blk].page_user_id[i];
	ASSERT(metadata->block_usage[to_blk].page_user_id[i] > 0);
//	ptr = find_user(metadata->block_usage[from_blk].page_user_id[i]);
//	ptr->using_pages_cnt++;
//	ptr->using_pages_per_sec++; //hu	
    }
#endif

    // finally, update the metadata
    	//*************************jian add***********************
	int using_pages = SSD_DATA_PAGES_PER_BLOCK(s);
	int plane_num_temp = metadata->block_usage[from_blk].plane_num;
	if(metadata->block_usage[from_blk].state == SSD_BLOCK_INUSE)
	{
		using_pages = 
		metadata->plane_meta[plane_num_temp].active_page[metadata->user_id] % s->params.pages_per_block;
	}

	plane_num_temp = metadata->block_usage[to_blk].plane_num;
   	if(metadata->plane_meta[plane_num_temp].free_pages[metadata->user_id] < using_pages)
    	{
             	metadata->plane_meta[plane_num_temp].free_pages[0] -= SSD_DATA_PAGES_PER_BLOCK(s);
             	metadata->plane_meta[plane_num_temp].using_nonallocating_pages[metadata->user_id] += SSD_DATA_PAGES_PER_BLOCK(s);
             	metadata->plane_meta[plane_num_temp].free_pages[metadata->user_id] += SSD_DATA_PAGES_PER_BLOCK(s);

		ASSERT(metadata->plane_meta[plane_num_temp].free_pages[0] >= 0);
    	}
        metadata->plane_meta[plane_num_temp].free_pages[metadata->user_id] -= using_pages;
    	//*************************jian add***********************
    metadata->block_usage[to_blk].bsn[metadata->user_id] = metadata->block_usage[from_blk].bsn[metadata->user_id];//*jian modify
    metadata->block_usage[to_blk].num_valid = metadata->block_usage[from_blk].num_valid;
    metadata->block_usage[from_blk].num_valid = 0;

    for (i = 0; i < s->params.pages_per_block; i ++) {
        int lpn = metadata->block_usage[from_blk].page[i];
        if (lpn != -1) {
            ASSERT(s->page_level_mapping_table[lpn] == ((from_blk * s->params.pages_per_block + i)
		 + (metadata->element_number * s->params.blocks_per_element * s->params.pages_per_block)));
            s->page_level_mapping_table[lpn] = (to_blk * s->params.pages_per_block + i)
		 + (metadata->element_number * s->params.blocks_per_element * s->params.pages_per_block);
        }
        metadata->block_usage[to_blk].page[i] = metadata->block_usage[from_blk].page[i];
	#ifdef DEBUG_VSSD
	metadata->block_usage[to_blk].page_user_id[i] = metadata->block_usage[from_blk].page_user_id[i];
	ASSERT(metadata->block_usage[to_blk].page_user_id[i] > 0);
	ptr = find_user(metadata->block_usage[from_blk].page_user_id[i]);
	ptr->using_pages_cnt++; //Jian
	ptr->using_pages_per_sec++;//hu
	#endif
    }
    metadata->block_usage[to_blk].state = metadata->block_usage[from_blk].state;
    //*jian add
    metadata->block_usage[to_blk].user_id = metadata->block_usage[from_blk].user_id;
    //*jian add

    bitpos = ssd_block_to_bitpos(s, to_blk);
    ssd_set_bit(metadata->free_blocks, bitpos);
    metadata->tot_free_blocks --;
    metadata->plane_meta[metadata->block_usage[to_blk].plane_num].free_blocks --;
#ifdef GC_SELF_BLOCKS
    int user_id = metadata->user_id;
    metadata->user_using_blocks[metadata->user_id]++;
    metadata->plane_meta[metadata->block_usage[to_blk].plane_num].user_using_blocks[metadata->user_id]++;
    /*ptr=find_user(user_id);//*jian
        if( ptr->using_block_cnt < ptr->aloc_log_block_cnt) //*jian if user self space is enough
        {
                ptr->using_block_cnt++;//*jian allocating self block
        }
        else if( (ptr->using_spare_block_cnt+ptr->using_block_cnt) < ptr->aloc_block_cnt ) //*jian if user self space is enough
        {
                ptr->using_spare_block_cnt++;//*jian allocating self block
        }
        else if( (ptr->using_spare_block_cnt+ptr->using_block_cnt) >= ptr->aloc_block_cnt )
        {
                user_time->using_block_cnt++;//*jian allocating spare blocks
                ptr->using_spare_block_cnt++;
        }
        else
                fprintf(stderr,"Error ! SSD space non-enough!");*/
#endif

#if SSD_ASSERT_ALL
    if (plane_num != -1) {
        f2 = ssd_free_bits(plane_num, elem_num, metadata, s);
        ASSERT(f2 == metadata->plane_meta[metadata->block_usage[to_blk].plane_num].free_blocks);
    }
#endif

    ssd_assert_free_blocks(s, metadata);
    ASSERT(metadata->block_usage[from_blk].num_valid == 0);

#if ASSERT_FREEBITS
    if (plane_num != -1) {
        f2 = ssd_free_bits(plane_num, elem_num, metadata, s);
        ASSERT(f1 == f2);
    }
#endif

    *mcost = cost;

    // stat
    metadata->tot_migrations ++;
    metadata->tot_pgs_migrated += metadata->block_usage[to_blk].num_valid;
    metadata->mig_cost += cost;

    return from_blk;
}

// return 1 if it is ok to clean this block
int ssd_pick_wear_aware_with_migration
(int blk, int block_life, double avg_lifetime, double *cost, int plane_num, int elem_num, ssd_t *s)
{
    int from_block = blk;
    double retirement_age;
    ASSERT(s->params.cleaning_policy == DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE);

    retirement_age = SSD_LIFETIME_THRESHOLD_Y * avg_lifetime;


    // see if this block's remaining lifetime is less than
    // the retirement threshold. if so, migrate cold data
    // into it.
    if (block_life < retirement_age) {

        // let us migrate some cold data into this block
        from_block = ssd_migrate_cold_data(blk, cost, plane_num, elem_num, s);
        //printf("Migrating frm blk %d to blk %d (blk life %d avg life %f\n",
        //  from_block, blk, block_life, avg_lifetime);
    }

    return from_block;
}

/*
 * a greedy solution, where we find the block in a plane with the least
 * num of valid pages and return it.
 */
static int ssd_pick_block_to_clean2(int plane_num, int elem_num, double *mcost, ssd_element_metadata *metadata, ssd_t *s)
{
    double avg_lifetime = 1;
    int i;
    int size;
    int block = -1;
    int min_valid = s->params.pages_per_block - 1; // one page goes for the summary info
	int min_valid_self = s->params.pages_per_block - 1;    	
    listnode *greedy_list;

    *mcost = 0;

    // find the average life time of all the blocks in this element
    avg_lifetime = ssd_compute_avg_lifetime(plane_num, elem_num, s);

    // we create a list of greedily selected blocks
    ll_create(&greedy_list);
    for (i = 0; i < s->params.blocks_per_element; i ++) {
        if (_ssd_pick_block_to_clean(i, plane_num, elem_num, metadata, s)) { //check if this block is belong to this user

            // greedily select the block
            if (metadata->block_usage[i].num_valid <= min_valid) {
                ASSERT(i == metadata->block_usage[i].block_num);
                ll_insert_at_head(greedy_list, (void*)&metadata->block_usage[i]); //it'a s global greedy list if disable GC_self block
                min_valid = metadata->block_usage[i].num_valid;
                block = i;
            }
        }
    } //end of for loop		

    if(block == -1)
    {
    	/*for (i = 0; i < s->params.blocks_per_element; i ++) 
	{
		fprintf(stderr,"(u %d) block[%d]=%d\t",metadata->block_usage[i].user_id,i,metadata->block_usage[i].num_valid);
		if(i % 20 == 0 )
		fprintf(stderr,"\n");
	}
	fprintf(stderr,"\n");
	fprintf(stderr,"(%d)free_pages=%d\t",0,metadata->free_pages[0]);
	fprintf(stderr,"(%d)free_pages=%d\t",1,metadata->free_pages[1]);
	fprintf(stderr,"(%d)free_pages=%d\n",2,metadata->free_pages[2]);
	fprintf(stderr,"(%d)using_pages=%d\t",1,metadata->using_nonallocating_pages[1]);
	fprintf(stderr,"(%d)using_pages=%d\n",2,metadata->using_nonallocating_pages[2]);
	fprintf(stderr,"ele_user_id = %d\n",metadata->user_id);
	fprintf(stderr,"elem_num = %d\t",elem_num);
	fprintf(stderr,"plane_num = %d\n",plane_num);*/
    }
    ASSERT(block != -1);
    block = -1;

    // from the greedily picked blocks, select one after rate
    // limiting the overly used blocks
    size = ll_get_size(greedy_list);

    //printf("plane %d elem %d size %d avg lifetime %f\n",
    //  plane_num, elem_num, size, avg_lifetime);
//hu modify
	for(i=0;i<size;i++) 
	{
		block_metadata *bm2;
		listnode *N = ll_get_nth_node(greedy_list, i);
		bm2 = ((block_metadata *)N->data); 		
		if(bm2->user_id == metadata->user_id) //it's this user's block
		{
			if(bm2->num_valid <= min_valid_self)
			{
				min_valid_self = bm2->num_valid; 
			}
		}
	}
//hu modify


    for (i = 0; i < size; i ++) {
        block_metadata *bm;
        int mig_blk;

        listnode *n = ll_get_nth_node(greedy_list, i);
        bm = ((block_metadata *)n->data);

        if (i == 0) {
            ASSERT(min_valid == bm->num_valid);
        }

        // this is the last of the greedily picked blocks.
        if (i == size -1) {
            // select it!
            block = bm->block_num;
            break;
        }

        if (s->params.cleaning_policy == DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AGNOSTIC) {
            block = bm->block_num;
            break;
        } else {
#if MIGRATE
            // migration
            mig_blk = ssd_pick_wear_aware_with_migration(bm->block_num, bm->rem_lifetime, avg_lifetime, mcost, bm->plane_num, elem_num, s);
            if (mig_blk != bm->block_num) {
                // data has been migrated and we have a new
                // block to use
                block = mig_blk;
                break;
            }
#endif

            // pick this block giving consideration to its life time
            if (ssd_pick_wear_aware(bm->block_num, bm->rem_lifetime, avg_lifetime, s)) {
                block = bm->block_num;
                break;
            }
        }
    }

    ll_release(greedy_list);

    ASSERT(block != -1);
    //fprintf(stderr, "want GC user_id = %d metadata->user_id =%d (simtime = %f)\n", metadata->block_usage[block].user_id , metadata->user_id,simtime);//*jian debug
    return block;
}

static int ssd_pick_block_to_clean1(int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    int i;
    int block = -1;
    int min_valid = s->params.pages_per_block - 1; // one page goes for the summary info

    for (i = 0; i < s->params.blocks_per_element; i ++) {
        if (_ssd_pick_block_to_clean(i, plane_num, elem_num, metadata, s)) {
            if (metadata->block_usage[i].num_valid < min_valid) {
                min_valid = metadata->block_usage[i].num_valid;
                block = i;
            }
        }
    }

    if (block != -1) {
    	ASSERT(metadata->block_usage[block].user_id > 0);//*jian add
        return block;
    } else {
        fprintf(stderr, "Error: we cannot find a block to clean in plane %d\n", plane_num);
        ASSERT(0);
        exit(1);
    }
}

static int ssd_pick_block_to_clean(int plane_num, int elem_num, double *mcost, ssd_element_metadata *metadata, ssd_t *s)
{
    return ssd_pick_block_to_clean2(plane_num, elem_num, mcost, metadata, s);
}

/*
 * this routine cleans one page at a time in a block. if all the
 * valid pages are moved, then the block is erased.
 */
static double _ssd_clean_block_partially(int plane_num, int elem_num, ssd_t *s)
{
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);
    plane_metadata *pm = &metadata->plane_meta[plane_num];
    double cost = 0;
    int block;
    int i;
    user_time_state *ptr;

    ASSERT(metadata->user_id_2>0);

    ptr = find_user(metadata->user_id_2);

    ASSERT(pm->clean_in_progress);
    block = pm->clean_in_block;

    #ifdef DEBUG_VSSD
    #ifdef CURREN_BLOCK_ENABLE
    #ifdef GC_SELF_BLOCKS
    	ASSERT(metadata->user_id == metadata->block_usage[block].user_id);
	if(metadata->block_usage[block].user_id!=metadata->user_id)
	{
		for(i=63;i>=0;i--)
		{
			if(metadata->block_usage[block].page_user_id[i] != -1)
			if(metadata->block_usage[block].page_user_id[i] != metadata->block_usage[block].user_id)
			{
				fprintf(stderr,"page_user_id = %d , blk_user_id = %d"\
				,metadata->block_usage[block].page_user_id[i],metadata->block_usage[block].user_id);
				ASSERT(0);
			}
		}
	}
    #endif
    #endif
    #endif

    if (metadata->block_usage[block].num_valid > 0) {
        // pick a page that is not yet cleaned and move it
        for (i = 0; i < s->params.pages_per_block; i ++) {
            int lp_num = metadata->block_usage[block].page[i];

            if (lp_num != -1) {
                cost += ssd_clean_one_page(lp_num, i, block, plane_num, elem_num, metadata, s);
		ptr->gc_live_page_copy_per_sec ++;
                break;
            }
        }
    }

    // if we've moved all the valid pages out of this block,
    // then we're done with it. so, erase it and update the
    // system state.
    if (metadata->block_usage[block].num_valid == 0) {
        // add the cost of erasing the block
        cost += s->params.block_erase_latency;

        ssd_update_free_block_status(block, plane_num, metadata, s);
        ssd_update_block_lifetime(simtime+cost, block, metadata);
        pm->clean_in_progress = 0;
        pm->clean_in_block = -1;

  	#ifdef DEBUG_VSSD
	for (i = 0; i < (s->params.pages_per_block-1); i ++) {
		user_time_state *ptr;
		ptr = find_user(metadata->block_usage[block].page_user_id[i]);
		ptr->using_pages_cnt --;
		//ptr->using_pages_per_sec--; //hu
		metadata->block_usage[block].page_user_id[i] = -1;
	}
	#endif
    }

    ptr->gc_overhead_time += cost;
    ptr->gc_overhead_cnt ++;
    return cost;
}

double ssd_clean_block_partially(int plane_num, int elem_num, ssd_t *s)
{
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);
    plane_metadata *pm = &metadata->plane_meta[plane_num];
    double cost = 0;
    double mcost = 0;

    // see if we've already started the cleaning
    if (!pm->clean_in_progress) {
        // pick a block to be cleaned
        pm->clean_in_block = ssd_pick_block_to_clean(plane_num, elem_num, &mcost, metadata, s);
        pm->clean_in_progress = 1;
    }

    // yes, the cleaning on this plane is already initiated
    ASSERT(pm->clean_in_block != -1);
    cost = _ssd_clean_block_partially(plane_num, elem_num, s);

    return (cost+mcost);
}


/*
 * this routine cleans one block by reading all its valid
 * pages and writing them to the current active block. the
 * cleaned block is also erased.
 */
double _ssd_clean_block_fully(int blk, int plane_num, int elem_num, ssd_element_metadata *metadata, ssd_t *s)
{
    double cost = 0;
    plane_metadata *pm = &metadata->plane_meta[plane_num];
    user_time_state *ptr = find_user(metadata->user_id_2);

    ASSERT((pm->clean_in_progress == 0) && (pm->clean_in_block = -1));
    pm->clean_in_block = blk;
    pm->clean_in_progress = 1;

    // stat
    pm->num_cleans ++;
    s->elements[elem_num].stat.num_clean ++;
    ptr->gc_issues_cnt ++;
    ptr->gc_issues_per_sec ++;

    do {
        cost += _ssd_clean_block_partially(plane_num, elem_num, s);
    } while (metadata->block_usage[blk].num_valid > 0);

    return cost;
}

static double ssd_clean_block_fully(int plane_num, int elem_num, ssd_t *s)
{
    int blk;
    double cost = 0;
    double mcost = 0;
    ssd_element_metadata *metadata = &s->elements[elem_num].metadata;
    plane_metadata *pm = &metadata->plane_meta[plane_num];

    ASSERT((pm->clean_in_progress == 0) && (pm->clean_in_block = -1));

    blk = ssd_pick_block_to_clean(plane_num, elem_num, &mcost, metadata, s);
    ASSERT(metadata->block_usage[blk].plane_num == plane_num);

    cost = _ssd_clean_block_fully(blk, plane_num, elem_num, metadata, s);
    return (cost+mcost);
}


static usage_table *ssd_build_usage_table(int elem_num, ssd_t *s)
{
    int i;
    usage_table *table;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    //////////////////////////////////////////////////////////////////////////////
    // allocate the hash table. it has (pages_per_block + 1) entries,
    // one entry for values from 0 to pages_per_block.
    table = (usage_table *) malloc(sizeof(usage_table) * (s->params.pages_per_block + 1));
    memset(table, 0, sizeof(usage_table) * (s->params.pages_per_block + 1));

    // find out how many blocks have a particular no of valid pages
    for (i = 0; i < s->params.blocks_per_element; i ++) {
        int usage = metadata->block_usage[i].num_valid;
        table[usage].len ++;
    }

    // allocate space to store the block numbers
    for (i = 0; i <= s->params.pages_per_block; i ++) {
        table[i].block = (int*)malloc(sizeof(int)*table[i].len);
    }

    /////////////////////////////////////////////////////////////////////////////
    // fill in the block numbers in their appropriate 'usage' buckets
    for (i = 0; i < s->params.blocks_per_element; i ++) {
        usage_table *entry;
        int usage = metadata->block_usage[i].num_valid;

        entry = &(table[usage]);
        entry->block[entry->temp ++] = i;
    }

    return table;
}

static void ssd_release_usage_table(usage_table *table, ssd_t *s)
{
    int i;

    for (i = 0; i <= s->params.pages_per_block; i ++) {
        free(table[i].block);
    }

    // release the table
    free(table);
}

/*
 * pick a random block with at least 1 empty page slot and clean it
 */
static double ssd_clean_blocks_random(int plane_num, int elem_num, ssd_t *s)
{
    double cost = 0;
#if 1
    printf("ssd_clean_blocks_random: not yet fixed\n");
    exit(1);
#else
    long blk = 0;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);

    do {
        // get a random number to select a block
        blk = DISKSIM_lrand48() % s->params.blocks_per_element;

        // if this is plane specific cleaning, then skip all the
        // blocks that don't belong to this plane.
        if ((plane_num != -1) && (metadata->block_usage[blk].plane_num != plane_num)) {
            continue;
        }

        // clean only those blocks that are used.
        if (ssd_can_clean_block(s, metadata, blk)) {

            int valid_pages = metadata->block_usage[blk].num_valid;

            // if all the pages in the block are valid, continue to
            // select another random block
            if (valid_pages == s->params.pages_per_block) {
                continue;
            } else {
                // invoke cleaning until we reach the high watermark
                cost += _ssd_clean_block_fully(blk, elem_num, metadata, s);

                if (ssd_stop_cleaning(plane_num, elem_num, s)) {
                    // we're done with creating enough free blocks. so quit.
                    break;
                }
            }
        } else { // block is already free. so continue.
            continue;
        }
    } while (1);
#endif

    return cost;
}

#define GREEDY_IN_COPYBACK 0

/*
 * first we create a hash table of blocks according to their
 * usage. then we select blocks with the least usage and clean
 * them.
 */
static double ssd_clean_blocks_greedy(int plane_num, int elem_num, ssd_t *s)
{
    double cost = 0;
    double avg_lifetime;
    int i;
    usage_table *table;
    ssd_element_metadata *metadata = &(s->elements[elem_num].metadata);


    /////////////////////////////////////////////////////////////////////////////
    // build the histogram
    table = ssd_build_usage_table(elem_num, s);

    //////////////////////////////////////////////////////////////////////////////
    // find the average life time of all the blocks in this element
    avg_lifetime = ssd_compute_avg_lifetime(plane_num, elem_num, s);

    /////////////////////////////////////////////////////////////////////////////
    // we now have a hash table of blocks, where the key of each
    // bucket is the usage count and each bucket has all the blocks with
    // the same usage count (i.e., the same num of valid pages).
    for (i = 0; i <= s->params.pages_per_block; i ++) {
        int j;
        usage_table *entry;

        // get the bucket of blocks with 'i' valid pages
        entry = &(table[i]);

        // free all the blocks with 'i' valid pages
        for (j = 0; j < entry->len; j ++) {
            int blk = entry->block[j];
            int block_life = metadata->block_usage[blk].rem_lifetime;

            // if this is plane specific cleaning, then skip all the
            // blocks that don't belong to this plane.
            if ((plane_num != -1) && (metadata->block_usage[blk].plane_num != plane_num)) {
                continue;
            }

            // if the block is already dead, skip it
            if (block_life == 0) {
                continue;
            }

            // clean only those blocks that are sealed.
            if (ssd_can_clean_block(s, metadata, blk)) {

                // if we care about wear-leveling, then we must rate limit overly cleaned blocks
                if (s->params.cleaning_policy == DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE) {

                    // see if this block's remaining lifetime is within
                    // a certain threshold of the average remaining lifetime
                    // of all blocks in this element
                    if (block_life < (SSD_LIFETIME_THRESHOLD_X * avg_lifetime)) {
                        // we have to rate limit this block as it has exceeded
                        // its cleaning limits
                        printf("Rate limiting block %d (block life %d avg life %f\n",
                            blk, block_life, avg_lifetime);

                        if (ssd_rate_limit(block_life, avg_lifetime)) {
                            // skip this block and go to the next one
                            continue;
                        }
                    }
                }

                // okies, finally here we're with the block to be cleaned.
                // invoke cleaning until we reach the high watermark.
                cost += _ssd_clean_block_fully(blk, metadata->block_usage[blk].plane_num, elem_num, metadata, s);

                if (ssd_stop_cleaning(plane_num, elem_num, s)) {
                    // no more cleaning is required -- so quit.
                    break;
                }
            }
        }

        if (ssd_stop_cleaning(plane_num, elem_num, s)) {
            // no more cleaning is required -- so quit.
            break;
        }
    }

    // release the table
    ssd_release_usage_table(table, s);

    // see if we were able to generate enough free blocks
    if (!ssd_stop_cleaning(plane_num, elem_num, s)) {
        printf("Yuck! we couldn't generate enough free pages in plane %d elem %d ssd %d\n",
            plane_num, elem_num, s->devno);
    }

    return cost;
}

double ssd_clean_element_no_copyback(int elem_num, ssd_t *s)
{
    double cost = 0;

    if (!ssd_start_cleaning(-1, elem_num, s)) {
        return cost;
    }

    switch(s->params.cleaning_policy) {
        case DISKSIM_SSD_CLEANING_POLICY_RANDOM:
            cost = ssd_clean_blocks_random(-1, elem_num, s);
            break;

        case DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AGNOSTIC:
        case DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE:
            cost = ssd_clean_blocks_greedy(-1, elem_num, s);
            break;

        default:
            fprintf(stderr, "Error: invalid cleaning policy %d\n",
                s->params.cleaning_policy);
            exit(1);
    }

    return cost;
}

double ssd_clean_plane_copyback(int plane_num, int elem_num, ssd_t *s)
{
    double cost = 0;

    ASSERT(plane_num != -1);

    switch(s->params.cleaning_policy) {
        case DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AGNOSTIC:
        case DISKSIM_SSD_CLEANING_POLICY_GREEDY_WEAR_AWARE:
            cost = ssd_clean_block_fully(plane_num, elem_num, s);
            break;

        case DISKSIM_SSD_CLEANING_POLICY_RANDOM:
        default:
            fprintf(stderr, "Error: invalid cleaning policy %d\n",
                s->params.cleaning_policy);
            exit(1);
    }

    return cost;
}

/*
 * 1. find a plane to clean in each of the parallel unit
 * 2. invoke copyback cleaning on all such planes simultaneously
 */
double ssd_clean_element_copyback(int elem_num, ssd_t *s)
{
    // this means that some (or all) planes require cleaning
    int clean_req = 0;
    int i;
    double max_cleaning_cost = 0;
    int plane_to_clean[SSD_MAX_PARUNITS_PER_ELEM];
    ssd_element_metadata *metadata = &s->elements[elem_num].metadata;
    int tot_cleans = 0;

    for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
        if ((plane_to_clean[i] = ssd_start_cleaning_parunit(i, elem_num, s)) != -1) {
            clean_req = 1;
        }
    }

    if (clean_req) {
        for (i = 0; i < SSD_PARUNITS_PER_ELEM(s); i ++) {
            double cleaning_cost = 0;
            int plane_num = plane_to_clean[i];

            if (plane_num == -1) {
                // don't force cleaning
                continue;
            }

            if (metadata->plane_meta[plane_num].clean_in_progress) {
                metadata->plane_meta[plane_num].clean_in_progress = 0;
                metadata->plane_meta[plane_num].clean_in_block = -1;
            }

            metadata->active_page[metadata->user_id] = metadata->plane_meta[plane_num].active_page[metadata->user_id];
            cleaning_cost = ssd_clean_plane_copyback(plane_num, elem_num, s);

            tot_cleans ++;

            if (max_cleaning_cost < cleaning_cost) {
                max_cleaning_cost = cleaning_cost;
            }
        }
    }

    return max_cleaning_cost;
}

double ssd_clean_element(ssd_t *s, int elem_num)
{
    double cost = 0;
    
    if (s->params.copy_back == SSD_COPY_BACK_DISABLE) {
        cost = ssd_clean_element_no_copyback(elem_num, s);
    } else {
        cost = ssd_clean_element_copyback(elem_num, s);
    }
    return cost;
}

int ssd_next_plane_in_parunit(int plane_num, int parunit_num, int elem_num, ssd_t *s)
{
    return (parunit_num*SSD_PLANES_PER_PARUNIT(s) + (plane_num+1)%SSD_PLANES_PER_PARUNIT(s));
}

int ssd_start_cleaning_parunit(int parunit_num, int elem_num, ssd_t *s)
{
    int i;
    int start;

    start = s->elements[elem_num].metadata.parunits[parunit_num].plane_to_clean;
    i = start;
    do {
        ASSERT(parunit_num == s->elements[elem_num].metadata.plane_meta[i].parunit_num);
        if (ssd_start_cleaning(i, elem_num, s)) {
            s->elements[elem_num].metadata.parunits[parunit_num].plane_to_clean = \
                ssd_next_plane_in_parunit(i, parunit_num, elem_num, s);
            return i;
        }

        i = ssd_next_plane_in_parunit(i, parunit_num, elem_num, s);
    } while (i != start);

    return -1;
}

/*
 * invoke cleaning when the number of free blocks drop below a
 * certain threshold (in a plane or an element).
 */
int ssd_start_cleaning(int plane_num, int elem_num, ssd_t *s)
{
	//*jian add
	#ifdef CURREN_BLOCK_ENABLE
	#ifdef GC_SELF_BLOCKS
	user_time_state *ptr;
	int user_id = s->elements[elem_num].metadata.user_id;
	ASSERT(user_id>0);
	
	int i;
	unsigned int free_blocks_t;
	int free_sum;
	//*jian add
        if(plane_num == -1)
	{
		free_blocks_t = s->elements[elem_num].metadata.tot_free_blocks;
		ptr = user_time->next;
                while(ptr!=user_time)
		{
			if(ptr->user_id != user_id)
			{
				free_sum = ptr->aloc_block_cnt/s->params.nelements;
				free_sum -= s->elements[elem_num].metadata.user_using_blocks[ptr->user_id];//find other user's logical free blocks
				if(free_sum>0)
					free_blocks_t -= free_sum;
			}

			ptr=ptr->next;
		}
		unsigned int low = (unsigned int)LOW_WATERMARK_PER_ELEMENT(s);
		ASSERT(0);
        	return (free_blocks_t < low);
	}
	else
	{
		//define in disksim_global.h
		#ifdef HU
		ptr = find_user(user_id);
		free_sum = s->elements[elem_num].metadata.plane_meta[plane_num].user_allocating_blocks[ptr->user_id];
		double min = (((s)->params.min_freeblks_percent)/100.0);
		int low = free_sum*min;
		free_sum -= s->elements[elem_num].metadata.plane_meta[plane_num].user_using_blocks[ptr->user_id]; 
		return (free_sum < low);
		#endif

		#ifndef HU
		free_blocks_t = s->elements[elem_num].metadata.plane_meta[plane_num].free_blocks;
		ptr = user_time->next;
                while(ptr!=user_time)
		{
			if(ptr->user_id != user_id)
			{
				//allocating log blocks (non-include remain blocks)
				//free_sum = ptr->aloc_block_cnt/(s->params.nelements*s->params.planes_per_pkg);
				free_sum = s->elements[elem_num].metadata.plane_meta[plane_num].user_allocating_blocks[ptr->user_id];
				//find other user's allocating free blocks +1 == current block
				free_sum -= s->elements[elem_num].metadata.plane_meta[plane_num].user_using_blocks[ptr->user_id];
				if(free_sum >= 0)
					free_blocks_t -= free_sum;//*keep other user's blocks
				else
				{
					if(s->elements[elem_num].metadata.plane_meta[plane_num].user_allocating_blocks[user_id]\
					>= s->elements[elem_num].metadata.plane_meta[plane_num].user_using_blocks[user_id])
					return 0;
				}
			}
			ptr=ptr->next;
		}
        	int low = (int)LOW_WATERMARK_PER_PLANE(s);
        	return (free_blocks_t < low);
		#endif
	}
	#endif
	#endif

    #ifndef GC_SELF_BLOCKS
    if (plane_num == -1) {
	#ifdef ALLOCATING_ELEMENT
	if(s->elements[elem_num].metadata.element_user_id != s->elements[elem_num].metadata.user_id_2)
	return 0;
	#endif
        unsigned int low = (unsigned int)LOW_WATERMARK_PER_ELEMENT(s);
        return (s->elements[elem_num].metadata.tot_free_blocks < low);
    } else {
	#ifdef ALLOCATING_ELEMENT
	if(s->elements[elem_num].metadata.element_user_id != s->elements[elem_num].metadata.user_id_2)
	return 0;
	#endif
	#ifdef ALLOCATING_PLANE
	if(s->elements[elem_num].metadata.plane_meta[plane_num].user_id != s->elements[elem_num].metadata.user_id)
	return 0;
	#endif
        int low = (int)LOW_WATERMARK_PER_PLANE(s);
        return (s->elements[elem_num].metadata.plane_meta[plane_num].free_blocks < low);
    }
    #endif
}

/*
 * stop cleaning when sufficient number of free blocks
 * are generated (in a plane or an element).
 */
int ssd_stop_cleaning(int plane_num, int elem_num, ssd_t *s)
{
	//*jian add
	#ifdef CURREN_BLOCK_ENABLE
	#ifdef GC_SELF_BLOCKS
	user_time_state *ptr;
	int user_id = s->elements[elem_num].metadata.user_id;
	
	int i;
	unsigned int free_blocks_t;
	int free_sum;
	//*jian add
	if(plane_num == -1)
        {
                free_blocks_t = s->elements[elem_num].metadata.tot_free_blocks;
		ptr = user_time->next;
                while(ptr!=user_time)
                {
			if(ptr->user_id != user_id)
			{
				free_sum = ptr->aloc_block_cnt/s->params.nelements;
				free_sum -= s->elements[elem_num].metadata.user_using_blocks[ptr->user_id];//find other user's logical free blocks
				if(free_sum>0)
					free_blocks_t -= free_sum;
			}

			ptr=ptr->next;
                }
        	unsigned int high = (unsigned int)HIGH_WATERMARK_PER_ELEMENT(s);
                return (free_blocks_t >= high);
        }
        else
        {
                free_blocks_t = s->elements[elem_num].metadata.plane_meta[plane_num].free_blocks;
		ptr = user_time->next;
                while(ptr!=user_time)
                {
			if(ptr->user_id != user_id)
			{
				//free_sum = ptr->aloc_block_cnt/(s->params.nelements*s->params.planes_per_pkg);
				free_sum = s->elements[elem_num].metadata.plane_meta[plane_num].user_allocating_blocks[ptr->user_id];
				//*jian =======================
				free_sum -= s->elements[elem_num].metadata.plane_meta[plane_num].user_using_blocks[ptr->user_id];//find other user's logical free blocks
				if(free_sum>0)
					free_blocks_t -= free_sum;
			}

			ptr=ptr->next;
                }
        	int high = (int)HIGH_WATERMARK_PER_PLANE(s);
                return (free_blocks_t >= high);
        }
	#endif
	#endif

    #ifndef GC_SELF_BLOCKS
    if (plane_num == -1) {
        unsigned int high = (unsigned int)HIGH_WATERMARK_PER_ELEMENT(s);
        return (s->elements[elem_num].metadata.tot_free_blocks >= high);
    } else {
        int high = (int)HIGH_WATERMARK_PER_PLANE(s);
        return (s->elements[elem_num].metadata.plane_meta[plane_num].free_blocks >= high);
    }
    #endif
}


