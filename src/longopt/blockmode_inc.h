/*--------------------------------------------------------------------
 *
 *	Copyright (c) 1991-2025 by the GMT Team (https://www.generic-mapping-tools.org/team.html)
 *	See LICENSE.TXT file for copying and redistribution conditions.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU Lesser General Public License as published by
 *	the Free Software Foundation; version 3 or any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU Lesser General Public License for more details.
 *
 *	Contact info: www.generic-mapping-tools.org
 *--------------------------------------------------------------------*/

#ifndef BLOCKMODE_INC_H
#define BLOCKMODE_INC_H

/* Translation table from long to short module options, directives and modifiers */

static struct GMT_KEYWORD_DICTIONARY module_kw[] = { /* Local options for all the block* modules */
    /* separator, short_option, long_option,
		  short_directives,    long_directives,
		  short_modifiers,     long_modifiers,
		  transproc_mask */
    { 0, 'A', "fields",
                  "z,s,l,h,w", "mode,L1_scale|l1_scale,low,high,weight",
                  "",          "",
                  GMT_TP_MULTIDIR },
    { 0, 'C', "center",    "", "", "", "", GMT_TP_STANDARD },
    { 0, 'D', "histogram",
                  "",          "",
                  "a,c,h,l",   "average,center,high,low",
		  GMT_TP_STANDARD },
    { 0, 'E', "extend",
                  "r,s",       "record,source",
                  "l,h",       "lower,higher",
		  GMT_TP_STANDARD },
    GMT_G_OUTGRID_KW,
    GMT_I_INCREMENT_KW,
    { 0, 'Q', "quick",     "", "", "", "", GMT_TP_STANDARD },
    { 0, 'W', "weights",
                  "i,o",       "in,out",
                  "s",         "sigma",
		  GMT_TP_STANDARD },
    { 0, '\0', "", "", "", "", "", 0 }  /* End of list marked with empty option and strings */
};

#endif  /* !BLOCKMODE_INC_H */
