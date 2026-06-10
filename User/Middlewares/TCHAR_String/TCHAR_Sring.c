/**
 * @file TCHAR_String.c
 * @brief UTF-16 (TCHAR) 与 UTF-8 字符串转换工具, 用于 FatFs 文件名处理
 *
 * ========== 为什么需要这个模块 ==========
 *
 * FatFs 的 TCHAR 类型在 _UNICODE 模式下为 UTF-16 编码 (每个字符 2 字节),
 * 可以支持长文件名中的中文字符、日文字符等 Unicode 字符。
 *
 * 而 LVGL 的文本显示和 printf 调试输出需要 UTF-8 编码 (多字节, 兼容 ASCII)。
 * 因此需要一个转换层, 在两种编码之间进行转换。
 *
 * ========== 提供的函数 ==========
 *
 *   TCHAR_Cmp       — 比较两个 TCHAR (UTF-16) 字符串是否相等
 *   TCHAR_Strrchr   — 在 TCHAR 字符串中反向查找指定字符 (如查找 '.' 扩展名)
 *   TCHAR_Strncpy   — 复制 TCHAR 字符串, 带长度限制
 *   TCHAR_ToUtf8    — 将 TCHAR (UTF-16) 字符串转换为 UTF-8 字节流
 *
 * ========== 编码转换说明 ==========
 *   UTF-16 → UTF-8 转换规则:
 *     U+0000 ~ U+007F:  1 字节, 0xxxxxxx
 *     U+0080 ~ U+07FF:  2 字节, 110xxxxx 10xxxxxx
 *     U+0800 ~ U+FFFF:  3 字节, 1110xxxx 10xxxxxx 10xxxxxx
 *   (本模块不处理 UTF-16 代理对, 即 > U+FFFF 的字符, 在 MP3 文件名中极少出现)
 *
 * @note 文件名 typo "Sring" 为历史遗留, 保持原名称不变以便搜索。
 */
#include "TCHAR_Sring.h"

/*
 * 函数: TCHAR_Cmp
 * 功能: 比较两个 TCHAR (UTF-16) 字符串
 *
 * 算法:
 *   1. 逐字符比较, 直到发现不匹配的字符
 *   2. 如果所有字符都匹配且同时到达结尾 (c1==c2==0), 返回 0
 *   3. 如果中途出现不匹配, 返回差值 (c1 - c2)
 *
 * @param s1 字符串 1
 * @param s2 字符串 2
 * @return   0 = 相等, >0 = s1>s2, <0 = s1<s2
 */
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

/*
 * 函数: TCHAR_Strrchr
 * 功能: 在 TCHAR 字符串中反向查找指定字符
 *
 * 遍历整个字符串, 记录最后一次匹配的位置。
 * 用于查找文件扩展名: TCHAR_Strrchr(fname, _T('.'))
 * 可以正确处理 "my.music.mp3" 这类多点的文件名。
 *
 * @param str 源字符串
 * @param ch  要查找的字符
 * @return    指向最后一次匹配位置的指针, 未找到返回 NULL
 */
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

/*
 * 函数: TCHAR_Strncpy
 * 功能: 复制 TCHAR 字符串, 带长度限制
 *
 * 与标准 strncpy 行为一致:
 *   从 src 复制最多 n-1 个字符到 dest
 *   如果 src 比 n 短, 剩余部分填充 (TCHAR)'\0'
 *
 * @param dest 目标缓冲区
 * @param src  源字符串
 * @param n    最大复制字符数
 * @return     dest 指针
 */
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

/*
 * 函数: TCHAR_ToUtf8
 * 功能: 将 TCHAR (UTF-16 LE) 字符串转换为 UTF-8 字节流
 *
 * 算法:
 *   逐字符读取 UTF-16 编码, 根据 Unicode 码点范围选择 1/2/3 字节输出:
 *
 *   U+0000 ~ U+007F  (ASCII):
 *     1 字节: 0xxxxxxx
 *     直接复制低 7 位
 *
 *   U+0080 ~ U+07FF  (如拉丁字母补充、希腊字母等):
 *     2 字节: 110xxxxx 10xxxxxx
 *     高 5 位 → 第一字节低 5 位, 低 6 位 → 第二字节低 6 位
 *
 *   U+0800 ~ U+FFFF  (如中日韩表意文字):
 *     3 字节: 1110xxxx 10xxxxxx 10xxxxxx
 *     高 4 位 → 第一字节低 4 位, 中间 6 位 → 第二字节, 低 6 位 → 第三字节
 *
 * 缓冲区安全检查:
 *   每次写入前检查剩余空间, 不足时返回 -1
 *
 * @param tstr     UTF-16 输入字符串
 * @param utf8_buf UTF-8 输出缓冲区
 * @param buf_size 输出缓冲区大小 (字节)
 * @return         UTF-8 字符串长度 (不含 null) 或 -1 (失败)
 */
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
            /*
             * 1字节 UTF-8: 0xxxxxxx
             * ASCII 字符, 直接复制
             */
            if (utf8_len + 1 > buf_size - 1)
            {
                return -1;
            }
            utf8_buf[utf8_len++] = (uint8_t)unicode;
        }
        else if (unicode <= 0x07FFU)
        {
            /*
             * 2字节 UTF-8: 110xxxxx 10xxxxxx
             *   byte1: 110 + 高 5 位
             *   byte2: 10 + 低 6 位
             */
            if (utf8_len + 2 > buf_size - 1)
            {
                return -1;
            }
            utf8_buf[utf8_len++] = (uint8_t)(0xC0U | (unicode >> 6));
            utf8_buf[utf8_len++] = (uint8_t)(0x80U | (unicode & 0x3FU));
        }
        else if (unicode <= 0xFFFFU)
        {
            /*
             * 3字节 UTF-8: 1110xxxx 10xxxxxx 10xxxxxx
             *   byte1: 1110 + 高 4 位
             *   byte2: 10 + 中 6 位
             *   byte3: 10 + 低 6 位
             */
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

    utf8_buf[utf8_len] = 0x00;  /* null-terminated */
    return (int32_t)utf8_len;
}
