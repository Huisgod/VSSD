#!/bin/bash
##########################
# VSSD disksim run  0.0
# 2012/02/17
##########################
PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin:~/bin
export PATH

#read -p "make file !? (y/n) : " make_check
#read -p "run cgdb !? (y/n) : " cgdb_check

run_ssdsim_cnt="4"
check_disksim_cnt=$(ps -lA | grep -c disksim)
while [ "$check_disksim_cnt" = "$run_ssdsim_cnt" ]
do
        echo "$check_disksim_cnt = $run_ssdsim_cnt \t Read Run: outputfile/F1_40m+Web1_32m-FIOSq_pageu.txt"
        sleep 30
        check_disksim_cnt=$(ps -lA | grep -c disksim)
done
echo "$check_disksim_cnt != $run_ssdsim_cnt\n Run disksim\n"

#===============intel-toolkid==================

#-------------------01 GC----------------------
#cp algorithm/local_gc.h src/disksim_global.h
#cp algorithm/global_gc.h src/disksim_global.h

#-----------------02 parallel------------------
#cp algorithm/credit_cost-request_response_time.h src/disksim_global.h
#cp algorithm/credit_cost-page_response_time.h src/disksim_global.h

#-------------03 device queuing delay----------
#cp algorithm/VSSD_with_queuing_delay.h src/disksim_global.h
#cp algorithm/VSSD_without_queuing_delay.h src/disksim_global.h

#==============================================

#------------------Run alone-------------------
#cp algorithm/Run_alone.h src/disksim_global.h

#--------------------Black BOX----------------------
#cp algorithm/black_box-FIOS.h src/disksim_global.h
#cp algorithm/black_box-request_response_time.h src/disksim_global.h

#--------------------White BOX----------------------
#cp algorithm/white_box-VSSDq_pageu.h src/disksim_global.h
#cp algorithm/white_box-VSSDq_blocku.h src/disksim_global.h
#cp algorithm/white_box-VSSDq_planeu.h src/disksim_global.h
#cp algorithm/white_box-VSSDq_dieu.h src/disksim_global.h



#if [ "$make_check" = "y" ] || [ "$make_check" = "Y" ]; then
#make clean
#make
#fi

	para=./ssdmodel/valid/Intel_toolkit.parv 
	input=../trace/01_0ran_8md_100rand_11m.dis
#	input=../trace/01_0ran_8md_0rand_11m.dis
	echo "input file : $input"
 	time ./src/disksim $para stdout  ascii $input  0 1> ./enable_borrow_dead_block.txt
#    cgdb --args ./src/disksim $para stdout  ascii $input  0

