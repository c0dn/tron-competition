#ifndef __TK_TKERNEL_H__
#define __TK_TKERNEL_H__

#include <stddef.h>
#include <stdint.h>

typedef uint8_t UB;
typedef uint32_t UW;
typedef int16_t H;
typedef int INT;
typedef INT ID;
typedef unsigned int UINT;
typedef int BOOL;
typedef INT ER;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef E_OK
#define E_OK 0
#endif

ER tk_set_flg(ID flgid, UINT ptn);

#endif /* __TK_TKERNEL_H__ */
