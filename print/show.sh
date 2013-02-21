#!/bin/bash
##########################
# VSSD disksim run  0.0
# 2012/02/17
##########################
PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin:~/bin
export PATH

tail -n 3 ../outputfile/*.txt
ls ../outputfile/
read -p "File : " file
#read -p "User_id : " user_id
#cat outputfile/$file | grep -w "$user_id)" | sed 's/^.*Throughput//g' | sed 's/MB.*$//g' #> show.txt
cat ../outputfile/$file | grep -w "1)" | sed 's/^.*Throughput//g' | sed 's/MB.*$//g' > user1.txt
cat ../outputfile/$file | grep -w "2)" | sed 's/^.*Throughput//g' | sed 's/MB.*$//g' > user2.txt
cat ../outputfile/$file | grep -w "3)" | sed 's/^.*Throughput//g' | sed 's/MB.*$//g' > user3.txt
