#ifndef SEMI_PLAYER_STATUS_H
#define SEMI_PLAYER_STATUS_H

/* 统一状态码（Api / C ABI / 命令完成结果）。
 * 约定见 docs/error_convention.md。
 * SEMI_OK == 0；其余为错误。数值稳定后勿随意重排（Dart 侧会写死）。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum semi_status {
    SEMI_OK = 0,
    SEMI_ERR_NOT_INITIALIZED = 1, /* 未 init 就调业务接口 */
    SEMI_ERR_INVALID_STATE = 2,   /* 会话状态不允许该操作 */
    SEMI_ERR_CANCELLED = 3,       /* 命令被 cancel */
    SEMI_ERR_ASSEMBLE_FAILED = 4, /* init / IoC 装配失败 */
    SEMI_ERR_INTERNAL = 5,        /* 未分类内部错误 */
    SEMI_ERR_INVALID_ARGUMENT = 6,/* 参数为空或超出合法范围 */
    SEMI_ERR_INVALID_HANDLE = 7   /* 句柄不存在、已释放或不支持该结果 */
} semi_status_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SEMI_PLAYER_STATUS_H */
