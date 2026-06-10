/**
 * @file TCHAR_String.h
 * @brief UTF-16 (TCHAR) 字符串工具函数头文件
 *
 * 提供 TCHAR 字符串比较、查找、复制和 UTF-8 转换接口。
 * 用于 FatFs Unicode 文件名与 LVGL 显示之间的编码桥接。
 */
#ifndef TCHAR_STRING_H
#define TCHAR_STRING_H

#include <stdint.h>
#include "main.h"
#include "fatfs.h"

/**
 * @brief 比较两个 TCHAR 宽字符串
 * @param s1  第一个字符串 [非空]
 * @param s2  第二个字符串 [非空]
 * @return 0=相等, <0=s1<s2, >0=s1>s2
 */
int TCHAR_Cmp(const TCHAR *s1, const TCHAR *s2);

/**
 * @brief 在 TCHAR 宽字符串中反向查找字符
 * @param str  源字符串 [非空]
 * @param ch   要查找的字符
 * @return 匹配位置的指针, 未找到返回 NULL
 */
TCHAR* TCHAR_Strrchr(const TCHAR *str, TCHAR ch);

/**
 * @brief 复制 TCHAR 宽字符串
 * @param dest  目标缓冲区 [非空, 长度 >= n]
 * @param src   源字符串 [非空]
 * @param n     最大拷贝字符数
 * @return dest 指针
 */
TCHAR* TCHAR_Strncpy(TCHAR *dest, const TCHAR *src, uint32_t n);

/**
 * @brief TCHAR(UTF-16) 转 UTF-8 字节流
 * @param tstr     源 TCHAR 宽字符串 [非空]
 * @param utf8_buf 输出 UTF-8 缓冲区 [非空]
 * @param buf_size 缓冲区容量 (字节)
 * @return 转换后的 UTF-8 字节数 (不含终止符), 失败返回 -1
 * @note 仅处理 BMP 平面 (U+0000~U+FFFF), 不支持代理对
 */
int32_t TCHAR_ToUtf8(const TCHAR *tstr, uint8_t *utf8_buf, uint32_t buf_size);

#endif /* TCHAR_STRING_H */
