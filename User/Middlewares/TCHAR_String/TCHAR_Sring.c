/**
 * @file TCHAR_String.c
 * @brief UTF-16 (TCHAR) 与 UTF-8 字符串转换工具, 用于 FatFs 文件名处理
 *
 * FatFs 的 TCHAR 类型在 _UNICODE 模式下为 UTF-16 编码(每个字符 2 字节),
 * 而 LVGL 和 printf 等显示模块需要 UTF-8 编码。
 * 本模块提供以下转换函数:
 *   - TCHAR_Cmp:      比较两个 TCHAR 字符串
 *   - TCHAR_Strrchr:  在 TCHAR 字符串中反向查找字符
 *   - TCHAR_Strncpy:  复制 TCHAR 字符串
 *   - TCHAR_ToUtf8:   TCHAR(UTF-16) 转 UTF-8 字节流
 *
 * 注意: 文件名 typo "Sring" 为历史遗留, 保持原名称不变。
 */
#include "TCHAR_Sring.h"

int TCHAR_Cmp(const TCHAR *s1, const TCHAR *s2)
{
    if ((s1 == NULL) || (s2 == NULL))
    {
        return -1;
    }

    while (1)
    {
        unsigned short c1 = (unsigned short)*s1++;
        unsigned short c2 = (unsigned short)*s2++;

        if (c1 != c2)
        {
            return (int)(c1 - c2);
        }

        if (c1 == 0)
        {
            return 0;
        }
    }
}

TCHAR* TCHAR_Strrchr(const TCHAR *str, TCHAR ch)
{
    if (str == NULL)
    {
        return NULL;
    }

    TCHAR *last_pos = NULL;

    while (*str != 0)
    {
        if (*str == ch)
        {
            last_pos = (TCHAR *)str;
        }
        str++;
    }

    return last_pos;
}

TCHAR* TCHAR_Strncpy(TCHAR *dest, const TCHAR *src, uint32_t n)
{
    if ((dest == NULL) || (src == NULL) || (n == 0))
    {
        return dest;
    }

    TCHAR *dest_ptr = dest;

    while ((n > 0) && (*src != (TCHAR)'\0'))
    {
        *dest_ptr++ = *src++;
        n--;
    }

    while (n > 0)
    {
        *dest_ptr++ = (TCHAR)'\0';
        n--;
    }

    return dest;
}

int32_t TCHAR_ToUtf8(const TCHAR *tstr, uint8_t *utf8_buf, uint32_t buf_size)
{
    if ((tstr == NULL) || (utf8_buf == NULL) || (buf_size == 0))
    {
        return -1;
    }

    uint32_t utf8_len = 0;
    uint16_t unicode;

    while ((unicode = (uint16_t)*tstr++) != 0)
    {
        if (unicode <= 0x007FU)
        {
            /* 1字节 UTF-8: 0xxxxxxx */
            if (utf8_len + 1 > buf_size - 1)
            {
                return -1;
            }
            utf8_buf[utf8_len++] = (uint8_t)unicode;
        }
        else if (unicode <= 0x07FFU)
        {
            /* 2字节 UTF-8: 110xxxxx 10xxxxxx */
            if (utf8_len + 2 > buf_size - 1)
            {
                return -1;
            }
            utf8_buf[utf8_len++] = (uint8_t)(0xC0U | (unicode >> 6));
            utf8_buf[utf8_len++] = (uint8_t)(0x80U | (unicode & 0x3FU));
        }
        else if (unicode <= 0xFFFFU)
        {
            /* 3字节 UTF-8: 1110xxxx 10xxxxxx 10xxxxxx */
            if (utf8_len + 3 > buf_size - 1)
            {
                return -1;
            }
            utf8_buf[utf8_len++] = (uint8_t)(0xE0U | (unicode >> 12));
            utf8_buf[utf8_len++] = (uint8_t)(0x80U | ((unicode >> 6) & 0x3FU));
            utf8_buf[utf8_len++] = (uint8_t)(0x80U | (unicode & 0x3FU));
        }
        else
        {
            return -1;
        }
    }

    utf8_buf[utf8_len] = 0x00;
    return (int32_t)utf8_len;
}
