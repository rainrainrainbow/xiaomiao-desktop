/**
 * @file poincare/runtime.h
 * @brief Poincaré - Script Engine: Runtime Interface
 * 
 * 参考 NumWorks Epsilon 的 Poincaré 层设计，提供统一的脚本引擎接口。
 * 当前实现基于 MicroPython，未来可扩展支持其他语言。
 */

#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化脚本引擎运行时
 * @param heap_size GC 堆大小（字节）
 * @return true 成功，false 失败
 */
bool poincare_runtime_init(size_t heap_size);

/**
 * @brief 销毁脚本引擎运行时
 */
void poincare_runtime_deinit(void);

/**
 * @brief 检查运行时是否已初始化
 * @return true 已初始化，false 未初始化
 */
bool poincare_runtime_is_ready(void);

/**
 * @brief 执行一段脚本代码
 * @param source 脚本源码（UTF-8）
 * @param source_name 源码名称（用于错误报告）
 * @return 0 成功，-1 失败
 */
int poincare_runtime_exec(const char *source, const char *source_name);

/**
 * @brief 执行一个脚本文件
 * @param filename 文件路径
 * @return 0 成功，-1 失败
 */
int poincare_runtime_exec_file(const char *filename);

/**
 * @brief 注册自定义模块
 * @param module_name 模块名称
 * @param init_func 模块初始化函数
 * @return true 成功，false 失败
 */
bool poincare_runtime_register_module(const char *module_name, void *init_func);

#endif /* RUNTIME_H */