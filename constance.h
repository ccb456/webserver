/**
 * 定义一些常量
 */

#ifndef CONSTANCE_H
#define CONSTANCE_H

/* 定义读缓冲区大小*/
const int READ_BUFF_SIZE = 2048;
const int WRITE_BUFF_SIZE = 1024;


/* main文件内的内容 */
const int MAX_FD = 65536;
const int MAX_EVENT_NUMBER = 10000;
const int TIMESLOT = 5;     // 时钟间隔时间，5s

/* DEBUG 下使用*/
#define debug
    

#endif