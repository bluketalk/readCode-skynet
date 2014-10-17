#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

struct skynet_config {
	int thread; //线程数量
	int harbor; //harbor id
	const char * logger;    //日志
	const char * module_path; //模块路径
	const char * master;    //master 地址
	const char * local;     //harbor 地址
	const char * start;     //启动文件（lua）
	const char * standalone;    //master配置 （配置了该项就说明这节点是master）
};

void skynet_start(struct skynet_config * config);

#endif
