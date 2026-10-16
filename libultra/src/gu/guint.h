/**************************************************************************
 *									  *
 *		 Copyright (C) 1994, Silicon Graphics, Inc.		  *
 *									  *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may  not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *									  *
 **************************************************************************/

#include "mbi.h"
#include "gu.h"

typedef union
{
	struct
	{
		unsigned int hi;
		unsigned int lo;
	} word;

	double	d;
} du;

typedef union
{
	unsigned int	i;
	float		f;
} fu;

#ifdef TARGET_PORT
/* The du initialisers list the high word first, which only matches the double's layout on big endian machines. */
static inline double du_value(du v)
{
	union { unsigned long long u; double d; } c;
	c.u = ((unsigned long long) v.word.hi << 32) | v.word.lo;
	return c.d;
}
#define DU_D(v) du_value(v)
#else
#define DU_D(v) ((v).d)
#endif

#ifndef __GL_GL_H__

typedef	float	Matrix[4][4];

#endif

#define ROUND(d)	(int)(((d) >= 0.0) ? ((d) + 0.5) : ((d) - 0.5))
#define	ABS(d)		((d) > 0) ? (d) : -(d)

extern float	__libm_qnan_f;
