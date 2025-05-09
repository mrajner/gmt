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

/*
 * Miscellaneous definitions and structures related to:
 * 1. Compass symbols used by pscbasemap and pscoast
 * 2. Custom symbols used by psxy and psxyz.
 * 3. Definitions for vector attributes
 *
 * Author: Paul Wessel
 * Date:	1-JAN-2010
 * Version:	6 API
 */

/*!
 * \file gmt_symbol.h
 * \brief Miscellaneous definitions and structures related to symbols
 */

#ifndef GMT_SYMBOLS_H
#define GMT_SYMBOLS_H

/* VECTOR attributes are used by psxy, psxyz, psrose, grdvector */
#define VECTOR_LINE_WIDTH	2.0	/* Default vector attributes in points */
#define VECTOR_HEAD_WIDTH	7.0
#define VECTOR_HEAD_LENGTH	9.0

/* PANEL attributes are used by pslegend, psscale, psimage, gmtlogo */

#define GMT_TAG_CLEARANCE	2.0	/* In points */
#define GMT_FRAME_CLEARANCE	4.0	/* In points */
#define GMT_FRAME_GAP		2.0	/* In points */
#define GMT_FRAME_RADIUS	6.0	/* In points */

/* Default width of a map roses are 15 and 10 % of map width */
#define GMT_MAG_ROSE_DEF_WIDTH	15.0
#define GMT_DIR_ROSE_DEF_WIDTH	10.0
/* Default size-depending embellishment offset (10% of size) */
#define GMT_EMBELLISHMENT_OFFSET	10.0

enum gmt_rose_mode {
	GMT_ROSE_SIZE_SET	= 0,	/* If size was given via +w */
	GMT_ROSE_SIZE_VAR	= 1,	/* Width set in percent */
	GMT_ROSE_OFF_SET	= 2,	/* Offset +o was set */
	GMT_ROSE_INT_SET	= 4		/* Intervals +t was set */
};

enum gmt_enum_panel {
	GMT_PANEL_INNER		= 1,
	GMT_PANEL_ROUNDED	= 2,
	GMT_PANEL_SHADOW	= 4,
	GMT_PANEL_FILL		= 8,
	GMT_PANEL_OUTLINE	= 16
};

enum gmt_enum_scale_orig {
	GMT_SCALE_ORIGIN_GIVEN = 0,
	GMT_SCALE_ORIGIN_PLACE = 1,
	GMT_SCALE_ORIGIN_MIDDLE = 2
};

/*! Definition of structure used for holding information about a reference point */
struct GMT_REFPOINT {	/* Used to hold items relevant for a reference point */
	double x;		/* X position of reference point */
	double y;		/* Y position of reference point */
	enum GMT_enum_refpoint mode;	/* Coordinate mode */
	int justify;		/* Justification integer (1-11) for reference point (if given via -Dj) */
	char *args;		/* Text representation of any additional arguments */
};

#define CUSTOM_SYMBOL_MAXVAR	3	/* So we can check in the code if we exceed this */

enum gmt_enum_custsymb {
	GMT_CONST_STRING	= -5,	/* We have a constant string in a conditional test */
	GMT_VAR_STRING		= -4,	/* We have a variable string (the trailing text) in a conditional test */
	GMT_VAR_SIZE		= -3,	/* We have the symbol size $s in a conditional test */
	GMT_VAR_IS_Y		= -2,	/* We have y or latitude in a conditional test */
	GMT_VAR_IS_X		= -1,	/* We have x or longitude in a conditional test */
	GMT_VAR_WORD		= 64,	/* We have a variable string (the trailing text) in a conditional test and we want a particular word */
	GMT_CONST_VAR		=  0,	/* We have a constant factor in a conditional test */
	GMT_BEGIN_SINGLE_IF	=  1,	/* We have a single, 1-liner if condition, with no end if */
	GMT_BEGIN_BLOCK_IF	=  2,	/* Starting a new if branch */
	GMT_END_IF		=  4,	/* Ending an if branch */
	GMT_END_IF_ELSE		=  6,	/* Ending an if-branch and start the else branch */
	GMT_BEGIN_ELSEIF	=  8	/* Ending the if-branch and start another if branch */
};

 struct GMT_CUSTOM_SYMBOL_EPS {
 	bool GMT_made;	/* True if the EPS was made by GMT modules */
 	bool placed;	/* True after we place the EPS code in the output PS file */
	char *name;		/* Name of the EPS macro file */
	char  *macro;	/* Contains all the EPS commands in one array */
 	double BB[4];	/* Will hold the BoundingBox as [x0 x1 y0 y1] */
};

struct GMT_CUSTOM_SYMBOL_ITEM {
	double x, y, p[CUSTOM_SYMBOL_MAXVAR], const_val[CUSTOM_SYMBOL_MAXVAR], var_sign[CUSTOM_SYMBOL_MAXVAR];
	int operator, var_pen, var[CUSTOM_SYMBOL_MAXVAR];	/* For conditionals: var[0] refers to variable on left hand side of operator, var[1] and var[2] to the right hand */
	enum gmt_enum_custsymb conditional;
	unsigned int justify;	/* For macro code l text justification [PSL_MC] */
	unsigned int angular;	/* 0 or GMT_SYMBOL_VARROTATE if constant angles given as azimuths */
	bool negate, is_var[CUSTOM_SYMBOL_MAXVAR];
	struct GMT_FILL *fill;
	struct GMT_PEN *pen;
	struct GMT_CUSTOM_SYMBOL_ITEM *next;
	struct GMT_FONT font;	/* Font to use for the l macro */
	struct GMT_CUSTOM_SYMBOL_EPS *eps;
	char action;
	char *string;
};

struct GMT_CUSTOM_SYMBOL {
	char          name[GMT_LEN64];	/* Name of this symbol (i.e., just the <name> in [<dir>/]<name>.def) */
	unsigned int  n_required;   /* Number of additional columns necessary to decode chosen symbol */
	unsigned int  start;    /* Column number of first additional column [2-4 depending on -C and psxy vs psxyz] */
	unsigned int  text;     /* >0 if symbol places text and hence need fonts to be set properly, 2 if using trailing text */
	unsigned int  *type;    /* Array with type of each parameter [0 = dimensionless, 1 = dimension, 2 = geographic angle (convert via projection)] */
	struct        GMT_CUSTOM_SYMBOL_ITEM *first;
};

/*! Plot a map panel behind scales, legends, images, logos */
struct GMT_MAP_PANEL {
	unsigned int mode;		/* 0 = rectangular, 1 = rounded, 2 = secondary frame, 4 = shade, 8 = fill, 16 = outline */
	double width, height;		/* Size of panel in inches */
	double padding[4];		/* Extend panel by this clearance (inches) in the w/e/s/n directions [0/0/0/0] */
	double radius;			/* Radius for rounded corner */
	double off[2];			/* Offset for background shaded rectangle (+s) */
	double gap;			/* Space between main and secondary frame */
	struct GMT_PEN pen1, pen2;	/* Pen for main and secondary frame outline */
	struct GMT_PEN debug_pen;	/* Pen for debug lines */
	struct GMT_FILL fill;		/* Frame fill */
	struct GMT_FILL sfill;		/* Background shade */
	bool clearance;			/* Used by pslegend since it has the -C option as well */
	bool debug;
};

/*! Plot a map inset box in psbasemap */
struct GMT_MAP_INSET {
	/* -D[g|j|n|x]<refpoint>+w<width>[/<height>][+j<justify>[+o<dx>[/<dy>]][+s<file>][+t] or [<unit>]<xmin>/<xmax>/<ymin>/<ymax>[r][+s<file>][+t] */
	int justify;		/* Gave center of inset */
	bool plot;		/* true if we want to draw the inset */
	bool oblique;		/* true if we want got <w/s/e/n>r instead of <w/e/s/n> */
	bool translate;		/* true if we want to translate plot origin to the LL corner of inset */
	char unit;		/* Unit of projected coordinates or 0 for geographic */
	struct GMT_REFPOINT *refpoint;
	double wesn[4];		/* Geographic or projected boundaries */
	double off[2];		/* Offset from reference point */
	double dim[2];		/* Width & height of box */
	char *file;			/* Used to write inset location and dimensions [+s] */
	struct GMT_MAP_PANEL *panel;	/* Everything about optional back panel */
};

/*! Plot a map scale in psbasemap and pscoast */
struct GMT_MAP_SCALE {
	struct GMT_REFPOINT *refpoint;
	double origin[2];	/* Longitude/latitude where scale should apply */
	double off[2];		/* Offset from reference point */
	double length;		/* How long the scale is in measure units */
	double z_scale;		/* How long the scale is in measure units */
	bool plot;		/* true if we want to draw the scale */
	bool fancy;		/* true for a fancy map scale */
	bool unit;		/* true if we should append distance unit to all annotations along the scale */
	bool do_label;		/* true if we should plot a label for the scale */
	bool old_style;		/* true if we are using old syntax, pre-panel settings */
	bool vertical;		/* Want a Cartesian vertical scale (i.e., for y-data) */
	bool zdata;		/* z-data vertical scale (i.e., for z-data in pswiggle) */
	int justify;		/* Justification of anchor point */
	int origin_mode;	/* 0 gave scale origin, 1 select same as placement point, 2 use mid-plot location for scale origin */
	char measure;		/* The unit, i.e., e|f|k|M|n|u */
	char alignment;		/* Placement of label: t(op), b(ottom), l(eft), r(ight) */
	char label[GMT_LEN128];	/* Alternative user-specified label */
	struct GMT_MAP_PANEL *panel;	/* Everything about optional back panel */
};

/*! Plot a map direction "rose" in psbasemap and pscoast */
struct GMT_MAP_ROSE {
	struct GMT_REFPOINT *refpoint;
	double size;		/* Diameter of the rose in measure units */
	double off[2];		/* Offset from reference point sensed by justify */
	double declination;	/* Magnetic declination if needed */
	double a_int[2];	/* Annotation interval for geographic and magnetic directions */
	double f_int[2];	/* Tick (large) interval for geographic and magnetic directions */
	double g_int[2];	/* Tick (small) interval for geographic and magnetic directions */
	bool plot;		/* true if we want to draw the rose */
	bool do_label;		/* true if we should plot labels for the rose */
	bool align;		/* true if we want labels readable from south [default rotates with projection] */
	bool draw_circle[2];	/* True if we should draw the circle(s) */
	int justify;		/* Gave justification of rose */
	unsigned int mode;	/* 0 for given width, 1 for percentage of map width [10%], 2 for offsets given */
	unsigned int type;	/* 0 for plain directional rose, 1 for a fancy directional map rose, 2 for magnetic rose */
	unsigned int kind;	/* 0 : 90 degrees, 1 : 45 degrees, 2 : 22.5 degrees between points */
	char label[4][GMT_LEN64];	/* User-changeable labels for W, E, S, N point */
	char dlabel[GMT_LEN256];	/* Magnetic declination label */
	struct GMT_PEN pen[2];	/* Pens for main and secondary magrose circle outline */
	struct GMT_MAP_PANEL *panel;	/* Everything about optional back panel */
};

#endif	/* GMT_SYMBOLS_H */
