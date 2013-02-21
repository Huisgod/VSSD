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
cp algorithm/local_gc.h src/disksim_global.h
#cp algorithm/global_gc.h src/disksim_global.h

#-----------------02 parallel------------------
#cp algorithm/credit_cost-request_response_time.h src/disksim_global.h
cp algorithm/credit_cost-page_response_time.h src/disksim_global.h

#-------------03 device queuing delay----------
#cp algorithm/VSSD_with_queuing_delay.h src/disksim_global.h
cp algorithm/VSSD_without_queuing_delay.h src/disksim_global.h

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

if [ 1 = 1 ]; then
	para=./ssdmodel/valid/Intel_toolkit.parv
	input=./ssdmodel/valid/01_0ran_8md_0rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_10rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_20rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_30rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_40rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_50rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_60rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_70rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_80rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_90rand_11m.dis
	input=./ssdmodel/valid/01_0ran_8md_100rand_11m.dis 
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/01_GC_problem_0rand_10rand-globalGC.txt
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/01_GC_problem_0rand_0rand-localGC.txt
	para=./ssdmodel/valid/Intel_toolkit.parv
	input=./ssdmodel/valid/02_01_100rand_100w_4kb_17m__100rand_100w_4kb_17m.dis
	input=./ssdmodel/valid/02_02_100rand_100w_4kb_17m__100rand_100w_8kb_18m.dis
	input=./ssdmodel/valid/02_03_100rand_100w_4kb_17m__100rand_100w_12kb_17m.dis
	input=./ssdmodel/valid/02_04_100rand_100w_4kb_17m__100rand_100w_16kb_15m.dis
	input=./ssdmodel/valid/02_05_100rand_100w_4kb_17m__100rand_100w_20kb_14m.dis
	input=./ssdmodel/valid/02_06_100rand_100w_4kb_17m__100rand_100w_24kb_13m.dis
	input=./ssdmodel/valid/02_07_100rand_100w_4kb_17m__100rand_100w_28kb_13m.dis
	input=./ssdmodel/valid/02_08_100rand_100w_4kb_17m__100rand_100w_32kb_13m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/02_parallem_problem_4KB_4KB-credit_cost_of_request.txt
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/02_parallem_problem_4KB_4KB-credit_cost_of_page.txt
	para=./ssdmodel/valid/Intel_toolkit.parv
	input=./ssdmodel/valid/03_100rand_100w_32kb_23m__100rand_100w_32kb_23m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/03_queuing_delay_problem_50%_50%-VSSDq_with_queuing_delay.txt
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/03_queuing_delay_problem_50%_50%-VSSDq_without_queuing_delay.txt

	para=./ssdmodel/valid/3_WebS.parv
	input=./ssdmodel/valid/3_WebS.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/3_Webs-VSSDq_blocku.txt

	para=./ssdmodel/valid/F1_40m+F2_100m.parv
	input=./ssdmodel/valid/F1_40m+F2_100m-F1.dis 
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+F2_100m-F1.txt
	input=./ssdmodel/valid/F1_40m+F2_100m-F2.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+F2_100m-F2.txt
	input=./ssdmodel/valid/F1_40m+F2_100m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+F2_100m-black_box-FIOS3.txt

	para=./ssdmodel/valid/F1_40m+Web1_32m.parv
	input=./ssdmodel/valid/F1_40m+Web1_32m-F1.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+Web1_32m-F1.txt
	input=./ssdmodel/valid/F1_40m+Web1_32m-Web1.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+Web1_32m-Web1.txt
	input=./ssdmodel/valid/F1_40m+Web1_32m.dis
	./src/disksim $para stdout  ascii $input  0 1> outputfile/F1_40m+Web1_32m-FIOS3.txt

	para=./ssdmodel/valid/Iozone_20m+sqlsim1_40m.parv
	input=./ssdmodel/valid/Iozone_20m+sqlsim1_40m-Iozone.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+sqlsim1_40m-Iozone.txt
	input=./ssdmodel/valid/Iozone_20m+sqlsim1_40m-sqlsim.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+sqlsim1_40m-Sqlsim.txt
	input=./ssdmodel/valid/Iozone_20m+sqlsim1_40m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+sqlsim1_40m-VSSDq_blocku.txt

	para=./ssdmodel/valid/Iozone_20m+Vmm_20m.parv
	input=./ssdmodel/valid/Iozone_20m+Vmm_20m-Iozone.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+Vmm_20m-Iozone.txt
	input=./ssdmodel/valid/Iozone_20m+Vmm_20m-Vmm.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+Vmm_20m-Vmm.txt
	input=./ssdmodel/valid/Iozone_20m+Vmm_20m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Iozone_20m+Vmm_20m-VSSDq_blocku.txt

	para=./ssdmodel/valid/Mp3_3m+Web2_50m.parv
	input=./ssdmodel/valid/Mp3_3m+Web2_50m-Mp3.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Mp3_3m+Web2_50m-Mp3.txt
	input=./ssdmodel/valid/Mp3_3m+Web2_50m-Web.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Mp3_3m+Web2_50m-Web.txt
	input=./ssdmodel/valid/Mp3_3m+Web2_50m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/Mp3_3m+Web2_50m-VSSDq_blocku.txt

	para=./ssdmodel/valid/vmm_20m+sqlsim_40m.parv
	input=./ssdmodel/valid/vmm_20m+sqlsim_40m-vmm.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/vmm_20m+sqlsim_40m-Vmm.txt
	input=./ssdmodel/valid/vmm_20m+sqlsim_40m-sqlsim.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/vmm_20m+sqlsim_40m-sqlsim.txt
	input=./ssdmodel/valid/vmm_20m+sqlsim_40m.dis
	#./src/disksim $para stdout  ascii $input  0 1> outputfile/vmm_20m+sqlsim_40m-VSSDq_blocku.txt
fi

	para=./ssdmodel/valid/Intel_toolkit.parv
	input=./ssdmodel/valid/01_0ran_8md_0rand_11m.dis
	#./src/disksim $para stdout  ascii $input 0 1> test.txt
#if [ "$cgdb_check" = "y" ] || [ "$cgdb_check" = "Y" ]; then
cgdb --args ./src/disksim $para ./output.txt ascii $input 0
#fi
