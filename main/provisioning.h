#pragma once

/*
 * 配置不完整时进入配网模式：开热点 + Web 配置页。
 * 用户保存成功后设备会自动重启，因此正常情况下本函数不返回。
 */
void provisioning_run(void);
