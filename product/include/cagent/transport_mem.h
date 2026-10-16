/* http.h 补充声明（内存后端）
 *
 * 放在独立头文件而不是 http.h：测试后端不属于产品 API，
 * 混进主头文件会让「产品有哪些能力」这件事变模糊。
 */

#ifndef CAGENT_TRANSPORT_MEM_H
#define CAGENT_TRANSPORT_MEM_H

#include <stddef.h>

#include "cagent/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 取内存传输后端。 */
const ca_transport_ops *ca_transport_mem(void);
const char *ca_transport_mem_name(void);

/* 预设要返回的数据。数据不会被复制——调用方需保证在 run 期间有效。
 * 没有对应的「读回数据」API：数据只经 on_data 流出。 */
void ca_mem_transport_set_payload(const unsigned char *data, size_t len, long status);

#ifdef __cplusplus
}
#endif

#endif /* CAGENT_TRANSPORT_MEM_H */