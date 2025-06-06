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
 * surface.c: a gridding program using splines in tension.
 * Reads xyz Cartesian triples and fits a surface to the data.
 * The surface satisfies (1 - T) D4 z - T D2 z = 0,
 * where D4 is the 2-D biharmonic operator, D2 is the
 * 2-D Laplacian, and T is a "tension factor" between 0 and 1.
 * End member T = 0 is the classical minimum curvature
 * surface.  T = 1 gives a harmonic surface.  Use T = 0.25
 * or so for potential data; something more for topography.
 *
 * Program includes over-relaxation for fast convergence and
 * automatic optimal grid factorization.
 *
 * See reference Smith & Wessel (Geophysics, 3, 293-305, 1990) for details.
 *
 * Authors:	Walter H. F. Smith and Paul Wessel
 * Date:	1-JAN-2010
 * Version:	6 API
 *
 * This 6.0 version is a complete re-write that differs from the original code:
 * 1. It uses scan-line grid structures, so we no longer need to transpose grids
 * 2. It keeps node spacing at 1, thus we no longer need complicated strides between
 *    active nodes.  That spacing is now always 1 and we expand the grid as we
 *    go to larger grids (i.e., adding more nodes).
 * 3. It relies more on functions and macros from GMT to handle rows/cols/node calculations.
 *
 * Note on KEYS: DD(= means -D takes an optional input Dataset as argument which may be followed by optional modifiers.
 *
 * Update for 6.4.0: We help users get a better interpolation by selecting the most optimal -R to
 * result in many intermediate grid spacings for the multigrid progression to provide the best
 * convergence, then shrink back to the requested region upon output.  Experts who wishes to defeat this
 * improvement can use -Qr which will honor the given -R exactly, even if prime.
 */

#include "gmt_dev.h"
#include "longopt/surface_inc.h"

#define THIS_MODULE_CLASSIC_NAME	"surface"
#define THIS_MODULE_MODERN_NAME	"surface"
#define THIS_MODULE_LIB		"core"
#define THIS_MODULE_PURPOSE	"Grid table data using adjustable tension continuous curvature splines"
#define THIS_MODULE_KEYS	"<D{,DD(=,LG(,GG}"
#define THIS_MODULE_NEEDS	"R"
#define THIS_MODULE_OPTIONS "-:RVabdefhiqrw" GMT_OPT("FH")

struct SURFACE_CTRL {
	struct SURFACE_A {	/* -A<aspect_ratio> */
		bool active;
		unsigned int mode;	/* 1 if given as fraction */
		double value;
	} A;
	struct SURFACE_C {	/* -C<converge_limit> */
		bool active;
		unsigned int mode;	/* 1 if given as fraction */
		double value;
	} C;
	struct SURFACE_D {	/* -D<line.xyz>[+d][+z[<zval>]] */
		bool active;
		bool debug;
		bool fix_z;
		double z;
		char *file;	/* Name of file with breaklines */
	} D;
	struct SURFACE_G {	/* -G<file> */
		bool active;
		char *file;
	} G;
	struct SURFACE_I {	/* -I (for checking only) */
		bool active;
	} I;
	struct SURFACE_J {	/* -G<file> */
		bool active;
		char *projstring;
	} J;
	struct SURFACE_L {	/* -Ll|u<limit> */
		bool active[2];
		char *file[2];
		double limit[2];
		unsigned int mode[2];
	} L;
	struct SURFACE_M {	/* -M<radius> */
		bool active;
		char *arg;
	} M;
	struct SURFACE_N {	/* -N<max_iterations> */
		bool active;
		unsigned int value;
	} N;
	struct SURFACE_Q {	/* -Q[r] */
		bool active;	/* Information sought */
		bool as_is;		/* Use -R exactly as given */
		bool adjusted;	/* Improved -R required pad changes */
		double wesn[4];	/* Best improved -R for this operation */
	} Q;
	struct SURFACE_S {	/* -S<radius>[m|s] */
		bool active;
		double radius;
		char unit;
	} S;
	struct SURFACE_T {	/* -T<tension>[i][b] */
		bool active[2];
		double b_tension, i_tension;
	} T;
	struct SURFACE_W {	/* -W[<logfile>] */
		bool active;
		char *file;
	} W;
	struct SURFACE_Z {	/* -Z<over_relaxation> */
		bool active;
		double value;
	} Z;
};

/* Various constants used in surface */

#define SURFACE_OUTSIDE			LONG_MAX	/* Index number indicating data is outside usable area */
#define SURFACE_CONV_LIMIT		0.0001		/* Default is 100 ppm of data range as convergence criterion */
#define SURFACE_MAX_ITERATIONS		500		/* Default iterations at final grid size */
#define SURFACE_OVERRELAXATION		1.4		/* Default over-relaxation value */
#define SURFACE_CLOSENESS_FACTOR	0.05		/* A node is considered known if the nearest data is within 0.05 of a gridspacing of the node */
#define SURFACE_IS_UNCONSTRAINED	0		/* Node has no data constraint within its bin box */
#define SURFACE_DATA_IS_IN_QUAD1	1		/* Nearnest data constraint is in quadrant 1 relative to current node */
#define SURFACE_DATA_IS_IN_QUAD2	2		/* Nearnest data constraint is in quadrant 2 relative to current node */
#define SURFACE_DATA_IS_IN_QUAD3	3		/* Nearnest data constraint is in quadrant 3 relative to current node */
#define SURFACE_DATA_IS_IN_QUAD4	4		/* Nearnest data constraint is in quadrant 4 relative to current node */
#define SURFACE_IS_CONSTRAINED		5		/* Node has already been set (either data constraint < 5% of grid size or during filling) */
#define SURFACE_UNCONSTRAINED		0		/* Use coefficients set for unconstrained node */
#define SURFACE_CONSTRAINED		1		/* Use coefficients set for constrained node */
#define SURFACE_BREAKLINE		1		/* Flag for breakline constraints that should overrule data constraints */

/* Misc. macros used to get row, cols, index, node, x, y, plane trend etc. */

/* Go from row, col to grid node location, accounting for the 2 boundary rows and columns: */
#define row_col_to_node(row,col,mx) ((uint64_t)(((int64_t)(row)+(int64_t)2)*((int64_t)(mx))+(int64_t)(col)+(int64_t)2))
/* Go from row, col to index array position, where no boundary rows and columns involved: */
#define row_col_to_index(row,col,n_columns) ((uint64_t)((int64_t)(row)*((int64_t)(n_columns))+(int64_t)(col)))
/* Go from data x to fractional column x: */
#define x_to_fcol(x,x0,idx) (((x) - (x0)) * (idx))
/* Go from x to grid integer column knowing it is a gridline-registered grid: */
#define x_to_col(x,x0,idx) ((int64_t)floor(x_to_fcol(x,x0,idx)+0.5))
/* Go from data y to fractional row y_up measured from south (y_up = 0) towards north (y_up = n_rows-1): */
#define y_to_frow(y,y0,idy) (((y) - (y0)) * (idy))
/* Go from y to row (row = 0 is north) knowing it is a gridline-registered grid: */
#define y_to_row(y,y0,idy,n_rows) ((n_rows) - 1 - x_to_col(y,y0,idy))
/* Go from col to x knowing it is a gridline-registered grid: */
#define col_to_x(col,x0,x1,dx,n_columns) (((int)(col) == (int)((n_columns)-1)) ? (x1) : (x0) + (col) * (dx))
/* Go from row to y knowing it is a gridline-registered grid: */
#define row_to_y(row,y0,y1,dy,n_rows) (((int)(row) == (int)((n_rows)-1)) ? (y0) : (y1) - (row) * (dy))
/* Evaluate the change in LS plane from (0,0) to (xx,y_up) (so in intercept involved): */
#define evaluate_trend(C,xx,y_up) (C->plane_sx * (xx) + C->plane_sy * (y_up))
/* Evaluate the LS plane at location (xx,y_up) (this includes the intercept): */
#define evaluate_plane(C,xx,y_up) (C->plane_icept + evaluate_trend (C, xx, y_up))
/* Extract col from index: */
#define index_to_col(index,n_columns) ((index) % (n_columns))
/* Extract row from index: */
#define index_to_row(index,n_columns) ((index) / (n_columns))

enum surface_nodes {	/* Node locations relative to current node, using compass directions */
	N2 = 0, NW, N1, NE, W2, W1, E1, E2, SW, S1, SE, S2 };	/* I.e., 0-11 */

/* The 4 indices per quadrant refer to points A-D in Figure A-1 in the reference for quadrant 1 */

static unsigned int p[5][4] = {	/* Indices into C->offset for each of the 4 quadrants, i.e., C->offset[p[quadrant][k]]], k = 0-3 */
	{ 0, 0, 0,  0},		/* This row is never used, so allows us to use quadrant 1-4 as first array index directly */
	{ NW, W1, S1, SE},	/* Indices for 1st quadrant */
	{ SW, S1, E1, NE},	/* Indices for 2nd quadrant */
	{ SE, E1, N1, NW},	/* Indices for 3rd quadrant */
	{ NE, N1, W1, SW}	/* Indices for 4th quadrant */
};

enum surface_bound { LO = 0, HI = 1 };

enum surface_limit { NONE = 0, DATA = 1, VALUE = 2, SURFACE = 3 };

enum surface_conv { BY_VALUE = 0, BY_PERCENT = 1 };

enum surface_iter { GRID_NODES = 0, GRID_DATA = 1 };

enum surface_tension { BOUNDARY = 0, INTERIOR = 1 };

struct SURFACE_DATA {	/* Data point and index to node it currently constrains  */
	gmt_grdfloat x, y, z;
	unsigned int kind;
	uint64_t index;
};

struct SURFACE_BRIGGS {		/* Coefficients in Taylor series for Laplacian(z) a la I. C. Briggs (1974)  */
	gmt_grdfloat b[6];
};

struct SURFACE_SEARCH {		/* Things needed inside compare function will be passed to QSORT_R */
	int current_nx;		/* Number of nodes in y-dir for a given grid factor */
	int current_ny;		/* Number of nodes in y-dir for a given grid factor */
	double inc[2];		/* Size of each grid cell for a given grid factor */
	double wesn[4];		/* Grid domain */
};

struct SURFACE_INFO {	/* Control structure for surface setup and execution */
	size_t n_alloc;			/* Number of data point positions allocated */
	uint64_t npoints;		/* Number of data points */
	uint64_t node_sw_corner;	/* Node index of southwest interior grid corner for current stride */
	uint64_t node_se_corner;	/* Node index of southeast interior grid corner for current stride */
	uint64_t node_nw_corner;	/* Node index of northwest interior grid corner for current stride */
	uint64_t node_ne_corner;	/* Node index of northeast interior grid corner for current stride */
	uint64_t n_empty;		/* No of unconstrained nodes at initialization  */
	uint64_t nxny;			/* Total number of grid nodes without boundaries  */
	uint64_t mxmy;			/* Total number of grid nodes with padding */
	uint64_t total_iterations;	/* Total iterations so far. */
	FILE *fp_log;			/* File pointer to log file, if -W is selected */
	struct SURFACE_DATA *data;	/* All the data constraints */
	struct SURFACE_BRIGGS *Briggs;	/* Array with Briggs 6-coefficients per nearest active data constraint */
	struct GMT_GRID *Grid;		/* The final grid */
	struct GMT_GRID *Bound[2];	/* Optional grids for lower and upper limits on the solution */
	struct GMT_GRID_HEADER *Bh;	/* Grid header for one of the limit grids [or NULL] */
	struct SURFACE_SEARCH info;	/* Information needed by the compare function passed to QSORT_R */
	unsigned int n_factors;		/* Number of factors in common for the dimensions (n_rows-1, n_columns-1) */
	unsigned int factors[32];	/* Array of these common factors */
	unsigned int set_limit[2];	/* For low and high: NONE = unconstrained, DATA = by min data value, VALUE = by user value, SURFACE by a grid */
	unsigned int max_iterations;	/* Max iterations per call to iterate */
	unsigned int converge_mode; 	/* BY_PERCENT if -C set fractional convergence limit [BY_VALUE] */
	unsigned int p[5][4];		/* Arrays with four nodes as function of quadrant in constrained fit */
	unsigned int q_pad[4];		/* Extra padding needed for constrain grids if wesn is extended */
	int current_stride;		/* Current node spacings relative to final spacing  */
	int previous_stride;		/* Previous node spacings relative to final spacing  */
	int n_columns;				/* Number of nodes in x-dir. (Final grid) */
	int n_rows;				/* Number of nodes in y-dir. (Final grid) */
	int mx;				/* Width of final grid including padding */
	int my;				/* Height of final grid including padding */
	int current_nx;			/* Number of nodes in x-dir for current stride */
	int current_ny;			/* Number of nodes in y-dir for current stride */
	int current_mx;			/* Number of current nodes in x-dir plus 4 extra columns */
	int previous_nx;		/* Number of nodes in x-dir for previous stride */
	int previous_ny;		/* Number of nodes in y-dir for previous stride */
	int previous_mx;		/* Number of current nodes in x-dir plus 4 extra columns */
	int current_mxmy;		/* Total number of grid nodes with padding */
	int offset[12];			/* Node-indices shifts of 12 nearby points relative center node */
	unsigned char *status;		/* Array with node status or quadrants */
	char mode_type[2];		/* D = include data points when iterating, I = just interpolate from larger grid */
	char format[GMT_BUFSIZ];	/* Format statement used in some messages */
	char *limit_file[2];		/* Pointers to grids with low and high limits, if selected */
	bool periodic;			/* true if geographic grid and west-east == 360 */
	bool constrained;		/* true if set_limit[LO] or set_limit[HI] is true */
	bool logging;			/* true if -W was specified */
	bool adjusted;			/* true if -L grids need to be enlarged with pads */
	double limit[2];		/* Low and high constrains on range of solution */
	double inc[2];			/* Size of each grid cell for current grid factor */
	double r_inc[2];		/* Reciprocal grid spacings  */
	double converge_limit;		/* Convergence limit */
	double radius;			/* Search radius for initializing grid  */
	double tension;			/* Tension parameter on the surface  */
	double boundary_tension;	/* Tension parameter at the boundary */
	double interior_tension;	/* Tension parameter in the interior */
	double z_mean;			/* Mean value of the data constraints z */
	double z_rms;			/* Root mean square range of z after removing planar trend  */
	double r_z_rms;			/* Reciprocal of z_rms (to avoid dividing) */
	double plane_icept;		/* Intercept of best fitting plane to data  */
	double plane_sx;		/* Slope of best fitting plane to data in x-direction */
	double plane_sy;		/* Slope of best fitting plane to data in y-direction */
	double *fraction;		/* Hold fractional increments of row and column used in fill_in_forecast */
	double coeff[2][12];		/* Coefficients for 12 nearby nodes, for constrained [0] and unconstrained [1] nodes */
	double relax_old, relax_new;	/* Coefficients for relaxation factor to speed up convergence */
	double wesn_orig[4];		/* Original -R domain as we might have shifted it due to -r */
	double alpha;			/* Aspect ratio dy/dx (1 for square pixels) */
	double a0_const_1, a0_const_2;	/* Various constants for off gridnode point equations */
	double alpha2, e_m2, one_plus_e2;
	double eps_p2, eps_m2, two_plus_ep2;
	double two_plus_em2;
};

GMT_LOCAL void surface_set_coefficients (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* These are the coefficients in the finite-difference expressions given
	 * by equations (A-4) [SURFACE_UNCONSTRAINED=0] and (A-7) [SURFACE_CONSTRAINED=1] in the reference.
	 * Note that the SURFACE_UNCONSTRAINED coefficients are normalized by a0 (20 for no tension/aspects)
	 * whereas the SURFACE_CONSTRAINED is used for a partial sum hence the normalization is done when the
	 * sum over the Briggs coefficients have been included in iterate. */
	double alpha4, loose, a0;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Set finite-difference coefficients [stride = %d]\n", C->current_stride);

	loose = 1.0 - C->interior_tension;
	C->alpha2 = C->alpha * C->alpha;
	alpha4 = C->alpha2 * C->alpha2;
	C->eps_p2 = C->alpha2;
	C->eps_m2 = 1.0 / C->alpha2;
	C->one_plus_e2 = 1.0 + C->alpha2;
	C->two_plus_ep2 = 2.0 + 2.0 * C->eps_p2;
	C->two_plus_em2 = 2.0 + 2.0 * C->eps_m2;

	C->e_m2 = 1.0 / C->alpha2;

	a0 = 1.0 / ( (6 * alpha4 * loose + 10 * C->alpha2 * loose + 8 * loose - 2 * C->one_plus_e2) + 4 * C->interior_tension * C->one_plus_e2);
	C->a0_const_1 = 2.0 * loose * (1.0 + alpha4);
	C->a0_const_2 = 2.0 - C->interior_tension + 2 * loose * C->alpha2;

	C->coeff[SURFACE_CONSTRAINED][W2]   = C->coeff[SURFACE_CONSTRAINED][E2]   = -loose;
	C->coeff[SURFACE_CONSTRAINED][N2]   = C->coeff[SURFACE_CONSTRAINED][S2]   = -loose * alpha4;
	C->coeff[SURFACE_UNCONSTRAINED][W2] = C->coeff[SURFACE_UNCONSTRAINED][E2] = -loose * a0;
	C->coeff[SURFACE_UNCONSTRAINED][N2] = C->coeff[SURFACE_UNCONSTRAINED][S2] = -loose * alpha4 * a0;
	C->coeff[SURFACE_CONSTRAINED][W1]   = C->coeff[SURFACE_CONSTRAINED][E1]   = 2 * loose * C->one_plus_e2;
	C->coeff[SURFACE_UNCONSTRAINED][W1] = C->coeff[SURFACE_UNCONSTRAINED][E1] = (2 * C->coeff[SURFACE_CONSTRAINED][W1] + C->interior_tension) * a0;
	C->coeff[SURFACE_CONSTRAINED][N1]   = C->coeff[SURFACE_CONSTRAINED][S1]   = C->coeff[SURFACE_CONSTRAINED][W1] * C->alpha2;
	C->coeff[SURFACE_UNCONSTRAINED][N1] = C->coeff[SURFACE_UNCONSTRAINED][S1] = C->coeff[SURFACE_UNCONSTRAINED][W1] * C->alpha2;
	C->coeff[SURFACE_CONSTRAINED][NW]   = C->coeff[SURFACE_CONSTRAINED][NE]   = C->coeff[SURFACE_CONSTRAINED][SW] =
		C->coeff[SURFACE_CONSTRAINED][SE] = -2 * loose * C->alpha2;
	C->coeff[SURFACE_UNCONSTRAINED][NW] = C->coeff[SURFACE_UNCONSTRAINED][NE] = C->coeff[SURFACE_UNCONSTRAINED][SW] =
		C->coeff[SURFACE_UNCONSTRAINED][SE] = C->coeff[SURFACE_CONSTRAINED][NW] * a0;

	C->alpha2 *= 2;		/* We will need these coefficients times two in the boundary conditions; do the doubling here  */
	C->e_m2   *= 2;
}

GMT_LOCAL void surface_set_offset (struct SURFACE_INFO *C) {
	/* The offset array holds the offset in 1-D index relative
	 * to the current node.  For movement along a row this is
	 * always -2, -1, 0, +1, +2 but along a column we move in
	 * multiples of current_mx, the extended grid row width,
	 * which is current_mx = current_nx + 4.
	 */
 	C->offset[N2] = -2 * C->current_mx;	/* N2: 2 rows above */
 	C->offset[NW] = -C->current_mx - 1;	/* NW: 1 row above and one column left */
 	C->offset[N1] = -C->current_mx;		/* N1: 1 row above */
 	C->offset[NE] = -C->current_mx + 1;	/* NE: 1 row above and one column right */
 	C->offset[W2] = -2;			/* W2: 2 columns left */
 	C->offset[W1] = -1;			/* W1 : 1 column left */
 	C->offset[E1] = +1;			/* E1 : 1 column right */
 	C->offset[E2] = +2;			/* E2 : 2 columns right */
 	C->offset[SW] = C->current_mx - 1;	/* SW : 1 row below and one column left */
 	C->offset[S1] = C->current_mx;		/* S1 : 1 row below */
 	C->offset[SE] = C->current_mx + 1;	/* SE : 1 row below and one column right */
 	C->offset[S2] = 2 * C->current_mx;	/* S2 : 2 rows below */
}

GMT_LOCAL void fill_in_forecast (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {

	/* Fills in bilinear estimates into new node locations after grid is expanded.
	   These new nodes are marked as unconstrained while the coarser data are considered
	   constraints in the next iteration.  We do this in two steps:
	     a) We sweep through the grid from last to first node and copy each node to the
	        new location due to increased grid dimensions.
	     b) Once nodes are in place we sweep through and apply the bilinear interpolation.
	 */

	uint64_t index_00, index_10, index_11, index_01, index_new, current_node, previous_node;
	int previous_row, previous_col, i, j, col, row, expand, first;
	unsigned char *status = C->status;
	double c, sx, sy, sxy, r_prev_size, c_plus_sy_dy, sx_plus_sxy_dy;
	gmt_grdfloat *u = C->Grid->data;

	/* First we expand the active grid to allow for more nodes. We do this by
	 * looping backwards from last node to first so that the locations we copy
	 * the old node values to will always have higher node number than their source.
	 * The previous grid solution has dimensions previous_nx x previous_ny while
	 * the new grid has dimensions current_nx x current_ny.  We thus loop over
	 * the old grid and place these nodes into the new grid.  */

	expand = C->previous_stride / C->current_stride;	/* Multiplicity of new nodes in both x and y dimensions */
	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Expand grid by factor of %d when going from stride = %d to %d\n", expand, C->previous_stride, C->current_stride);

	for (previous_row = C->previous_ny - 1; previous_row >= 0; previous_row--) {	/* Loop backward over the previous grid rows */
		row = previous_row * expand;	/* Corresponding row in the new extended grid */
		for (previous_col = C->previous_nx - 1; previous_col >= 0; previous_col--) {	/* Loop backward over previous grid cols */
			col = previous_col * expand;	/* Corresponding col in the new extended grid */
			current_node  = row_col_to_node (row, col, C->current_mx);			/* Current node index */
			previous_node = row_col_to_node (previous_row, previous_col, C->previous_mx);	/* Previous node index */
			C->Grid->data[current_node] = C->Grid->data[previous_node];			/* Copy the value over */
		}
	}

	/* The active grid has now increased in size and the previous values have been copied to their new nodes.
	 * The grid nodes in-between these new "constrained" nodes are partly filled with old values (since
	 * we just copied, not moved, the nodes) or zeros (since we expanded the grid into new unused memory).
	 * This does not matter since we will now fill in those in-between nodes with a bilinear interpolation
	 * based on the coarser (previous) nodes.  At the end all nodes in the active grid are valid, except
	 * in the boundary rows/cols.  These are reset by set_BC before the iteration starts.
	 */

	/* Precalculate the fractional increments of rows and cols in-between the old constrained rows and cols.
	 * These are all fractions between 0 and 1.  E.g., if we quadruple the grid dimensions in x and y then
	 * expand == 4 and we need 4 fractions = {0, 0.25, 0.5, 0.75}. */

	r_prev_size = 1.0 / (double)C->previous_stride;
	for (i = 0; i < expand; i++) C->fraction[i] = i * r_prev_size;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Fill in expanded grid by bilinear interpolation [stride = %d]\n", C->current_stride);

	/* Loop over 4-point "bin squares" from the first northwest bin to the last southeast bin. The bin vertices are the expanded previous nodes */

	for (previous_row = 1; previous_row < C->previous_ny; previous_row++) {	/* Starts at row 1 since it is the baseline for the bin extending up to row 0 (north) */
		row = previous_row * expand;	/* Corresponding row in the new extended grid */

		for (previous_col = 0; previous_col < (C->previous_nx-1); previous_col++) {	/* Stop 1 column short of east since east is the right boundary of last bin */
			col = previous_col * expand;	/* Corresponding col in the new extended grid */

			/* Get the indices of the bilinear square defined by nodes {00, 10, 11, 01}, with 00 referring to the current (lower left) node */
			index_00 = row_col_to_node (row, col, C->current_mx);	/* Lower left corner of square bin and our origin */
			index_01 = index_00 - expand * C->current_mx;		/* Upper left corner of square bin */
			index_10 = index_00 + expand;				/* Lower right corner of square bin */
			index_11 = index_01 + expand;				/* Upper right corner of square bin */

			/* Get bilinear coefficients for interpolation z = c + sx * delta_x + sy * delta_y + sxy * delta_x * delta_y,
			 * which we will use as z = (c + sy * delta_y) + delta_x * (sx + sxy * delta_y).
			 * Below, delta_x and delta_y are obtained via C->fraction[i|j] that we pre-calculated above. */
			c = u[index_00];	sx = u[index_10] - c;
			sy = u[index_01] - c;	sxy = u[index_11] - u[index_10] - sy;

			/* Fill in all the denser nodes except the lower-left starting point */

			for (j = 0, first = 1; j < expand; j++) {	/* Set first = 1 so we skip the first column when j = 0 */
				c_plus_sy_dy = c + sy * C->fraction[j];	/* Compute terms that remain constant for this j */
				sx_plus_sxy_dy = sx + sxy * C->fraction[j];
				index_new = index_00 - j * C->current_mx + first;	/* Start node on this intermediate row */
				for (i = first;  i < expand; i++, index_new++) {	/* Sweep across this row and interpolate */
					u[index_new] = (gmt_grdfloat)(c_plus_sy_dy + C->fraction[i] * sx_plus_sxy_dy);
					status[index_new] = SURFACE_IS_UNCONSTRAINED;	/* These are considered temporary estimates */
				}
				first = 0;	/* Reset to 0 for the remainder of the j loop */
			}
			status[index_00] = SURFACE_IS_CONSTRAINED;	/* The previous node values will be kept fixed in the next iterate call */
		}
	}

	/* The loops above exclude the north and east boundaries.  First do linear interpolation along the east edge */

	index_00 = C->node_ne_corner;	/* Upper NE node */
	for (previous_row = 1; previous_row < C->previous_ny; previous_row++) {	/* So first edge is from row = 1 up to row = 0 on eastern edge */
		index_01 = index_00;			/* Previous lower becomes current upper node */
		index_00 += expand * C->current_mx;	/* Lower node after striding down */
		sy = u[index_01] - u[index_00];		/* Vertical gradient in u toward ymax (for increasing j) */
		index_new = index_00 - C->current_mx;	/* Since we start at j = 1 we skip up one row here */
		for (j = 1; j < expand; j++, index_new -= C->current_mx) {	/* Start at 1 since we skip the constrained index_00 node */
			u[index_new] = u[index_00] + (gmt_grdfloat)(C->fraction[j] * sy);
			status[index_new] = SURFACE_IS_UNCONSTRAINED;	/* These are considered temporary estimates */
		}
		status[index_00] = SURFACE_IS_CONSTRAINED;	/* The previous node values will be kept fixed in the next iterate call */
	}
	/* Next do linear interpolation along the north edge */
	index_10 = C->node_nw_corner;	/* Left NW node */
	for (previous_col = 0; previous_col < (C->previous_nx-1); previous_col++) {	/* To ensure last edge ends at col = C->previous_nx-1 */
		index_00 = index_10;		/* Previous right node becomes current left node */
		index_10 = index_00 + expand;	/* Right node after striding to the right */
		sx = u[index_10] - u[index_00];	/* Horizontal gradient in u toward xmax (for increasing i) */
		index_new = index_00 + 1;	/* Start at 1 since we skip the constrained index_00 node */
		for (i = 1; i < expand; i++, index_new++) {
			u[index_new] = u[index_00] + (gmt_grdfloat)(C->fraction[i] * sx);
			status[index_new] = SURFACE_IS_UNCONSTRAINED;	/* These are considered temporary estimates */
		}
		status[index_00] = SURFACE_IS_CONSTRAINED;	/* The previous node values will be kept fixed in the next iterate call */
	}
	/* Finally set the northeast corner to be considered fixed in the next iterate call and our work here is done */
	status[C->node_ne_corner] = SURFACE_IS_CONSTRAINED;
}

#ifdef QSORT_R_THUNK_FIRST
/* thunk arg is first argument to compare function */
GMT_LOCAL int surface_compare_points (void *arg, const void *point_1v, const void *point_2v) {
#else
/* thunk arg is last argument to compare function */
GMT_LOCAL int surface_compare_points (const void *point_1v, const void *point_2v, void *arg) {
#endif
	/* Routine for QSORT_R to sort data structure for fast access to data by node location.
	   Sorts on index first, then on radius to node corresponding to index, so that index
	   goes from low to high, and so does radius.  Note: These are simple Cartesian distance
	 * calculations.  The metadata needed to do the calculations are passed via *arg.
	*/
	uint64_t col, row, index_1, index_2;
	double x0, y0, dist_1, dist_2;
	const struct SURFACE_DATA *point_1 = point_1v, *point_2 = point_2v;
	struct SURFACE_SEARCH *info;
	index_1 = point_1->index;
	index_2 = point_2->index;
	if (index_1 < index_2) return (-1);
	if (index_1 > index_2) return (+1);
	if (index_1 == SURFACE_OUTSIDE) return (0);
	/* Points are in same grid cell.  First check for breakline points to sort those ahead of data points */
	if (point_1->kind == SURFACE_BREAKLINE && point_2->kind == 0) return (-1);
	if (point_2->kind == SURFACE_BREAKLINE && point_1->kind == 0) return (+1);
	/* Now find the one who is nearest to grid point */
	/* Note: index calculations do not include boundary pad */
	info = arg;	/* Get the needed metadata for distance calculations */
	row = index_to_row (index_1, info->current_nx);
	col = index_to_col (index_1, info->current_nx);
	x0 = col_to_x (col, info->wesn[XLO], info->wesn[XHI], info->inc[GMT_X], info->current_nx);
	y0 = row_to_y (row, info->wesn[YLO], info->wesn[YHI], info->inc[GMT_Y], info->current_ny);
	dist_1 = (point_1->x - x0) * (point_1->x - x0) + (point_1->y - y0) * (point_1->y - y0);
	/* Try to speed things up by first checking if point_2 x-distance from x0 alone exceeds point_1's radial distance */
	dist_2 = (point_2->x - x0) * (point_2->x - x0);	/* Just dx^2 */
	if (dist_1 < dist_2) return (-1);	/* Don't need to consider the y-distance */
	/* Did not exceed, so now we must finalize the dist_2 calculation by including the y-separation */
	dist_2 += (point_2->y - y0) * (point_2->y - y0);
	if (dist_1 < dist_2) return (-1);
	if (dist_1 > dist_2) return (+1);
	return (0);
}

GMT_LOCAL void surface_smart_divide (struct SURFACE_INFO *C) {
	/* Divide grid by its next largest prime factor and shift that setting by one */
	C->current_stride /= C->factors[C->n_factors - 1];
	C->n_factors--;
}

GMT_LOCAL void surface_set_index (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* Recomputes data[k].index for the new value of the stride,
	   sorts the data again on index and radii, and throws away
	   data which are now outside the usable limits.
	   Note: These indices exclude the padding. */
	int col, row;
	uint64_t k, k_skipped = 0;
	struct GMT_GRID_HEADER *h = C->Grid->header;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Recompute data index for next iteration [stride = %d]\n", C->current_stride);

	for (k = 0; k < C->npoints; k++) {
		col = (int)x_to_col (C->data[k].x, h->wesn[XLO], C->r_inc[GMT_X]);
		row = (int)y_to_row (C->data[k].y, h->wesn[YLO], C->r_inc[GMT_Y], C->current_ny);
		if (col < 0 || col >= C->current_nx || row < 0 || row >= C->current_ny) {
			C->data[k].index = SURFACE_OUTSIDE;
			k_skipped++;
		}
		else
			C->data[k].index = row_col_to_index (row, col, C->current_nx);
	}

	QSORT_R (C->data, C->npoints, sizeof (struct SURFACE_DATA), surface_compare_points, &(C->info));

	C->npoints -= k_skipped;
}

GMT_LOCAL void surface_solve_Briggs_coefficients (struct SURFACE_INFO *C, gmt_grdfloat *b, double xx, double yy, gmt_grdfloat z) {
	/* Given the normalized offset (xx,yy) from current node (value z) we determine the
	 * Briggs coefficients b_k, k = 1,5  [Equation (A-6) in the reference]
	 * Here, xx, yy are the fractional distances, accounting for any anisotropy.
	 * Note b[5] initially contains the sum of the 5 Briggs coefficients but
	 * we actually need to divide by it so we do that change here as well.
	 * Finally, b[4] will be multiplied with the off-node constraint so we do that here.
	 */
	double xx2, yy2, xx_plus_yy, xx_plus_yy_plus_one, inv_xx_plus_yy_plus_one, inv_delta, b_4;

	xx_plus_yy = xx + yy;
	xx_plus_yy_plus_one = 1.0 + xx_plus_yy;
	inv_xx_plus_yy_plus_one = 1.0 / xx_plus_yy_plus_one;
	xx2 = xx * xx;	yy2 = yy * yy;
	inv_delta = inv_xx_plus_yy_plus_one / xx_plus_yy;
	b[0] = (gmt_grdfloat)((xx2 + 2.0 * xx * yy + xx - yy2 - yy) * inv_delta);
	b[1] = (gmt_grdfloat)(2.0 * (yy - xx + 1.0) * inv_xx_plus_yy_plus_one);
	b[2] = (gmt_grdfloat)(2.0 * (xx - yy + 1.0) * inv_xx_plus_yy_plus_one);
	b[3] = (gmt_grdfloat)((-xx2 + 2.0 * xx * yy - xx + yy2 + yy) * inv_delta);
	b_4 = 4.0 * inv_delta;
	/* We also need to normalize by the sum of the b[k] values, so sum them here */
	b[5] = b[0] + b[1] + b[2] + b[3] + (gmt_grdfloat)b_4;
	/* We need to sum k = 0<5 of u[k]*b[k], where u[k] are the nodes of the points A-D,
	 * but the k = 4 point (E) is our data constraint.  We multiply that in here, once,
	 * add add b[4] to the rest of the sum inside the iteration loop. */
	b[4] = (gmt_grdfloat)(b_4 * z);

	/* b[5] is part of a denominator so we do the division here instead of inside iterate loop */
	b[5] = (gmt_grdfloat)(1.0 / (C->a0_const_1 + C->a0_const_2 * b[5]));
}

GMT_LOCAL void surface_find_nearest_constraint (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* Determines the nearest data point per bin and sets the
	 * Briggs parameters or, if really close, fixes the node value */
	uint64_t k, last_index, node, briggs_index, node_final;
	openmp_int row, col;
	double xx, yy, x0, y0, dx, dy;
	gmt_grdfloat z_at_node, *u = C->Grid->data;
	unsigned char *status = C->status;
	struct GMT_GRID_HEADER *h = C->Grid->header;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Determine nearest point and set Briggs coefficients [stride = %d]\n", C->current_stride);

	gmt_M_grd_loop (GMT, C->Grid, row, col, node) {	/* Reset status of all interior grid nodes */
		status[node] = SURFACE_IS_UNCONSTRAINED;
	}

	last_index = UINTMAX_MAX;	briggs_index = 0U;

	for (k = 0; k < C->npoints; k++) {	/* Find constraining value  */
		if (C->data[k].index != last_index) {	/* Moving to the next node to address its nearest data constraint */
			/* Note: Index calculations do not consider the boundary padding */
			row = (openmp_int)index_to_row (C->data[k].index, C->current_nx);
			col = (openmp_int)index_to_col (C->data[k].index, C->current_nx);
			last_index = C->data[k].index;	/* Now this is the last unique index we worked on */
	 		node = row_col_to_node (row, col, C->current_mx);
			/* Get coordinates of this node */
			x0 = col_to_x (col, h->wesn[XLO], h->wesn[XHI], C->inc[GMT_X], C->current_nx);
			y0 = row_to_y (row, h->wesn[YLO], h->wesn[YHI], C->inc[GMT_Y], C->current_ny);
			/* Get offsets dx,dy of data point location relative to this node (dy is positive up) in fraction of grid increments */
			dx = x_to_fcol (C->data[k].x, x0, C->r_inc[GMT_X]);
			dy = y_to_frow (C->data[k].y, y0, C->r_inc[GMT_Y]);
			/* So dx, dy are here fractions of C->inc[GMT_X] and C-inc[GMT_Y] */
			/* "Really close" will mean within 5% of the current grid spacing from the center node */

	 		if (fabs (dx) < SURFACE_CLOSENESS_FACTOR && fabs (dy) < SURFACE_CLOSENESS_FACTOR) {	/* Considered close enough to assign fixed value to node */
	 			status[node] = SURFACE_IS_CONSTRAINED;
	 			/* Since data constraint is forcibly moved from (dx, dy) to (0,0) we must adjust for
	 			 * the small change in the planar trend between the two locations, and then
	 			 * possibly clip the value if constraining surfaces were given.  Note that
	 			 * dx, dy is in -1/1 range normalized by (current_x|y_inc) so to recover the
	 			 * corresponding dx,dy in units of current grid fractions we must scale both
				 * dx and dy by current_stride; this is equivalent to scaling the trend.
				 * This trend then is normalized by dividing by the z rms.*/

	 			z_at_node = C->data[k].z + (gmt_grdfloat) (C->r_z_rms * C->current_stride * evaluate_trend (C, dx, dy));
	 			if (C->constrained) {	/* Must use final spacing node index to access the Bound grids */
					node_final = gmt_M_ijp (C->Bh, C->current_stride * row, C->current_stride * col);
					if (C->set_limit[LO] && !gmt_M_is_fnan (C->Bound[LO]->data[node_final]) && z_at_node < C->Bound[LO]->data[node_final])
						z_at_node = C->Bound[LO]->data[node_final];
					else if (C->set_limit[HI] && !gmt_M_is_fnan (C->Bound[HI]->data[node_final]) && z_at_node > C->Bound[HI]->data[node_final])
						z_at_node = C->Bound[HI]->data[node_final];
	 			}
	 			u[node] = z_at_node;
	 		}
	 		else {	/* We have a nearby data point in one of the quadrants */
				/* Note: We must swap dx,dy for 2nd and 4th quadrants and always use absolute values since we are
				   rotating other cases (quadrants 2-4) to look like quadrant 1 */
	 			if (dy >= 0.0) {	/* Upper two quadrants */
		 			if (dx >= 0.0) {	/* Both positive, use as is */
	 					status[node] = SURFACE_DATA_IS_IN_QUAD1;
						xx = dx;	yy = dy;
					}
	 				else {	/* dx negative, so remove sign, and swap */
	 					status[node] = SURFACE_DATA_IS_IN_QUAD2;
						yy = -dx;	xx = dy;
					}
	 			}
	 			else {	/* Lower two quadrants where we need to remove sign from dy */
		 			if (dx >= 0.0) {	/* Also swap x and y */
	 					status[node] = SURFACE_DATA_IS_IN_QUAD4;
						yy = dx;	xx = -dy;
					}
	 				else {	/* Just remove both signs */
	 					status[node] = SURFACE_DATA_IS_IN_QUAD3;
						xx = -dx;	yy = -dy;
					}
				}
				/* Evaluate the Briggs coefficients */
				surface_solve_Briggs_coefficients (C, C->Briggs[briggs_index].b, xx, yy, C->data[k].z);
	 			briggs_index++;
	 		}
	 	}
	 }
}

GMT_LOCAL void surface_set_grid_parameters (struct SURFACE_INFO *C) {
	/* Set the previous settings to the current settings */
	C->previous_nx = C->current_nx;
	C->previous_mx = C->current_mx;
	C->previous_ny = C->current_ny;
	/* Update the current parameters given the new C->current_stride setting */
	C->info.current_nx = C->current_nx = (C->n_columns - 1) / C->current_stride + 1;
	C->info.current_ny = C->current_ny = (C->n_rows - 1) / C->current_stride + 1;
	C->current_mx = C->current_nx + 4;
	C->current_mxmy = C->current_mx * (C->current_ny + 4);	/* Only place where "my" is used */
	C->info.inc[GMT_X] = C->inc[GMT_X] = C->current_stride * C->Grid->header->inc[GMT_X];
	C->info.inc[GMT_Y] = C->inc[GMT_Y] = C->current_stride * C->Grid->header->inc[GMT_Y];
	C->r_inc[GMT_X] = 1.0 / C->inc[GMT_X];
	C->r_inc[GMT_Y] = 1.0 / C->inc[GMT_Y];
	/* Update the grid node indices of the 4 corners */
	C->node_nw_corner = 2 * C->current_mx + 2;
	C->node_sw_corner = C->node_nw_corner + (C->current_ny - 1) * C->current_mx;
	C->node_se_corner = C->node_sw_corner + C->current_nx - 1;
	C->node_ne_corner = C->node_nw_corner + C->current_nx - 1;
}

GMT_LOCAL void surface_initialize_grid (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* For the initial gridsize, compute weighted averages of data inside the search radius
	 * and assign the values to u[col,row], where col,row are multiples of gridsize.
	 * Weights are Gaussian, i.e., this is a MA Gaussian filter operation.
	 */
	uint64_t index_1, index_2, k, k_index, node;
	int del_col, del_row, col, row, col_min, col_max, row_min, row_max, ki, kj;
	double r, rfact, sum_w, sum_zw, weight, x0, y0;
	gmt_grdfloat *u = C->Grid->data;
	struct GMT_GRID_HEADER *h = C->Grid->header;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Initialize grid using moving average scheme [stride = %d]\n", C->current_stride);

	del_col = irint (ceil (C->radius / C->inc[GMT_X]));
	del_row = irint (ceil (C->radius / C->inc[GMT_Y]));
	rfact = -4.5 / (C->radius*C->radius);
 	for (row = 0; row < C->current_ny; row++) {
		y0 = row_to_y (row, h->wesn[YLO], h->wesn[YHI], C->inc[GMT_Y], C->current_ny);
		for (col = 0; col < C->current_nx; col++) {
			/* For this node on the grid, find all data points within the radius */
			x0 = col_to_x (col, h->wesn[XLO], h->wesn[XHI], C->inc[GMT_X], C->current_nx);
	 		col_min = col - del_col;
	 		if (col_min < 0) col_min = 0;
	 		col_max = col + del_col;
	 		if (col_max >= C->current_nx) col_max = C->current_nx - 1;
	 		row_min = row - del_row;
	 		if (row_min < 0) row_min = 0;
	 		row_max = row + del_row;
	 		if (row_max >= C->current_ny) row_max = C->current_ny - 1;
			index_1 = row_col_to_index (row_min, col_min, C->current_nx);
			index_2 = row_col_to_index (row_max, col_max+1, C->current_nx);
	 		sum_w = sum_zw = 0.0;
	 		k = 0;
	 		while (k < C->npoints && C->data[k].index < index_1) k++;
			/* This double loop visits all nodes within the rectangle of dimensions (2*del_col by 2*del_row) centered on x0,y0 */
 			for (kj = row_min; k < C->npoints && kj <= row_max && C->data[k].index < index_2; kj++) {
	 			for (ki = col_min; k < C->npoints && ki <= col_max && C->data[k].index < index_2; ki++) {
					k_index = row_col_to_index (kj, ki, C->current_nx);
	 				while (k < C->npoints && C->data[k].index < k_index) k++;
	 				while (k < C->npoints && C->data[k].index == k_index) {
	 					r = (C->data[k].x-x0)*(C->data[k].x-x0) + (C->data[k].y-y0)*(C->data[k].y-y0);
						if (r > C->radius) continue;	/* Outside the circle */
	 					weight = exp (rfact * r);
	 					sum_w  += weight;
	 					sum_zw += weight * C->data[k].z;
	 					k++;
	 				}
	 			}
	 		}
			node = row_col_to_node (row, col, C->current_mx);
	 		if (sum_w == 0.0) {
	 			sprintf (C->format, "No data inside search radius at: %s %s [node set to data mean]\n", GMT->current.setting.format_float_out, GMT->current.setting.format_float_out);
	 			GMT_Report (GMT->parent, GMT_MSG_WARNING, C->format, x0, y0);
	 			u[node] = (gmt_grdfloat)C->z_mean;
	 		}
	 		else
	 			u[node] = (gmt_grdfloat)(sum_zw/sum_w);
		}
	}
}

GMT_LOCAL int surface_read_data (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, struct GMT_OPTION *options) {
	/* Process input data into data structure */
	int col, row, error;
	uint64_t k = 0, kmax = 0, kmin = 0, n_dup = 0;
	double *in, half_dx, zmin = DBL_MAX, zmax = -DBL_MAX, wesn_lim[4];
	struct GMT_GRID_HEADER *h = C->Grid->header;
	struct GMT_RECORD *In = NULL;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Processing input table data\n");
	C->data = gmt_M_memory (GMT, NULL, C->n_alloc, struct SURFACE_DATA);

	/* Read in xyz data and computes index no and store it in a structure */

	if ((error = GMT_Set_Columns (GMT->parent, GMT_IN, 3, GMT_COL_FIX_NO_TEXT)) != GMT_NOERROR)
		return (error);
	if (GMT_Init_IO (GMT->parent, GMT_IS_DATASET, GMT_IS_POINT, GMT_IN, GMT_ADD_DEFAULT, 0, options) != GMT_NOERROR)	/* Establishes data input */
		return (GMT->parent->error);

	C->z_mean = 0.0;
	/* Initially allow points to be within 1 grid spacing of the grid */
	wesn_lim[XLO] = h->wesn[XLO] - C->inc[GMT_X];	wesn_lim[XHI] = h->wesn[XHI] + C->inc[GMT_X];
	wesn_lim[YLO] = h->wesn[YLO] - C->inc[GMT_Y];	wesn_lim[YHI] = h->wesn[YHI] + C->inc[GMT_Y];
	half_dx = 0.5 * C->inc[GMT_X];

	if (GMT_Begin_IO (GMT->parent, GMT_IS_DATASET, GMT_IN, GMT_HEADER_ON) != GMT_NOERROR)	/* Enables data input and sets access mode */
		return (GMT->parent->error);

	do {	/* Keep returning records until we reach EOF */
		if ((In = GMT_Get_Record (GMT->parent, GMT_READ_DATA, NULL)) == NULL) {	/* Read next record, get NULL if special case */
			if (gmt_M_rec_is_error (GMT)) 		/* Bail if there are any read errors */
				return (GMT_RUNTIME_ERROR);
			else if (gmt_M_rec_is_eof (GMT)) 		/* Reached end of file */
				break;
			continue;	/* Go back and read the next record */
		}

		if (In->data == NULL) {
			gmt_quit_bad_record (GMT->parent, In);
			return (GMT->parent->error);
		}

		/* Data record to process */
		in = In->data;	/* Only need to process numerical part here */

		if (gmt_M_is_dnan (in[GMT_Z])) continue;	/* Cannot use NaN values */
		if (gmt_M_y_is_outside (GMT, in[GMT_Y], wesn_lim[YLO], wesn_lim[YHI])) continue; /* Outside y-range (or latitude) */
		if (gmt_x_is_outside (GMT, &in[GMT_X], wesn_lim[XLO], wesn_lim[XHI]))  continue; /* Outside x-range (or longitude) */
		row = (int)y_to_row (in[GMT_Y], h->wesn[YLO], C->r_inc[GMT_Y], C->current_ny);
		if (row < 0 || row >= C->current_ny) continue;
		if (C->periodic && ((h->wesn[XHI]-in[GMT_X]) < half_dx)) {	/* Push all values to the western nodes */
			in[GMT_X] -= 360.0;	/* Make this point constrain the western node value and then duplicate to east later */
			col = 0;
		}
		else	/* Regular point not at the periodic boundary */
			col = (int)x_to_col (in[GMT_X], h->wesn[XLO], C->r_inc[GMT_X]);
		if (col < 0 || col >= C->current_nx) continue;

		C->data[k].index = row_col_to_index (row, col, C->current_nx);
		C->data[k].x = (gmt_grdfloat)in[GMT_X];
		C->data[k].y = (gmt_grdfloat)in[GMT_Y];
		C->data[k].z = (gmt_grdfloat)in[GMT_Z];
		/* Determine the mean, min and max z-values */
		if (zmin > in[GMT_Z]) zmin = in[GMT_Z], kmin = k;
		if (zmax < in[GMT_Z]) zmax = in[GMT_Z], kmax = k;
		C->z_mean += in[GMT_Z];
		if (++k == C->n_alloc) {
			C->n_alloc <<= 1;
			C->data = gmt_M_memory (GMT, C->data, C->n_alloc, struct SURFACE_DATA);
		}
		if (C->periodic && col == 0) {	/* Now we must replicate information from the western to the eastern boundary */
			col = C->current_nx - 1;
			C->data[k].index = row_col_to_index (row, col, C->current_nx);
			C->data[k].x = (gmt_grdfloat)(in[GMT_X] + 360.0);
			C->data[k].y = (gmt_grdfloat)in[GMT_Y];
			C->data[k].z = (gmt_grdfloat)in[GMT_Z];
			C->z_mean += in[GMT_Z];
			if (++k == C->n_alloc) {
				C->n_alloc <<= 1;
				C->data = gmt_M_memory (GMT, C->data, C->n_alloc, struct SURFACE_DATA);
			}
			n_dup++;
		}
	} while (true);


	if (GMT_End_IO (GMT->parent, GMT_IN, 0) != GMT_NOERROR)	/* Disables further data input */
		return (GMT->parent->error);

	C->npoints = k;	/* Number of data points that passed being "inside" the grid region */

	if (C->npoints == 0) {
		GMT_Report (GMT->parent, GMT_MSG_ERROR, "No datapoints inside region, aborting\n");
		gmt_M_free (GMT, C->data);
		return (GMT_RUNTIME_ERROR);
	}

	C->z_mean /= C->npoints;	/* Estimate mean data value */
	if (gmt_M_is_verbose (GMT, GMT_MSG_INFORMATION)) {
		char msg[GMT_LEN256] = {""};
		sprintf (C->format, "%s %s %s\n", GMT->current.setting.format_float_out, GMT->current.setting.format_float_out, GMT->current.setting.format_float_out);
		sprintf (msg, C->format, (double)C->data[kmin].x, (double)C->data[kmin].y, (double)C->data[kmin].z);
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Minimum value of your dataset x,y,z at: %s", msg);
		sprintf (msg, C->format, (double)C->data[kmax].x, (double)C->data[kmax].y, (double)C->data[kmax].z);
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Maximum value of your dataset x,y,z at: %s", msg);
		if (C->periodic && n_dup) GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Number of input values shared between repeating west and east column nodes: %" PRIu64 "\n", n_dup);
	}
	C->data = gmt_M_memory (GMT, C->data, C->npoints, struct SURFACE_DATA);

	if (C->set_limit[LO] == DATA)	/* Wanted to set lower limit based on minimum observed z value */
		C->limit[LO] = C->data[kmin].z;
	else if (C->set_limit[LO] == VALUE && C->limit[LO] > C->data[kmin].z)
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Your lower value is > than min data value.\n");
	if (C->set_limit[HI] == DATA)	/* Wanted to set upper limit based on maximum observed z value */
		C->limit[HI] = C->data[kmax].z;
	else if (C->set_limit[HI] == VALUE && C->limit[HI] < C->data[kmax].z)
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Your upper value is < than max data value.\n");
	return (0);
}

GMT_LOCAL void surface_set_NaN (struct GMT_CTRL *GMT, struct GMT_GRID *G, unsigned int r0, unsigned int r1, unsigned int c0, unsigned int c1) {
	/* Set a subset of a grid to NaN based on rows and columns specified, inclusive.
	 * e.g, in Matlab this would be G->data(r0:r1,c0:c1) = NaN */
	uint64_t ij = gmt_M_ijp (G->header, r0, c0);	/* Top left node in block */
	unsigned int r, c;	/* Row and column loop variables */
	for (r = r0; r <= r1; r++)	/* For all the rows to work on: r0 up to and including r1 */
		for (c = c0; c <= c1; c++)	/* For all the columns to work on: c0 up top and including c1 */
			G->data[ij+(r-r0)*G->header->mx+(c-c0)] = GMT->session.f_NaN;
}

GMT_LOCAL void surface_enlarge_constraint_grid (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, struct GMT_GRID *G) {
	/* We must enlarge the grid region after having read the grid as is.  Then we need to set the new nodes
	 * not part of the constraint file to NaN. */

	gmt_grd_pad_on (GMT, G, C->q_pad);	/* First add in the larger pad to adjust the size of the grid */
	gmt_M_memcpy (G->header->wesn, C->Grid->header->wesn, 4U, double);	/* Next enlarge the region in the header */
	gmt_M_grd_setpad (GMT, G->header, C->Grid->header->pad);	/* Reset to standard pad (2/2/2/2) */
	gmt_set_grddim (GMT, G->header);	/* Update all dimensions to reflect the revised region and pad*/
	/* Now the grid has the right shape as the interior surface solution grid.  We now need to set the new
	 * nodes to NaN - to do this we consult C->q_pad[side] > 2 */
	if (C->q_pad[XLO] > 2)	/* We extended the grid westwards so need to set the first columns on the left to NaN */
		surface_set_NaN (GMT, G, 0, G->header->n_rows - 1, 0, C->q_pad[XLO] - 3);
	if (C->q_pad[XHI] > 2)	/* We extended the grid eastwards so need to set the last columns on the right to NaN */
		surface_set_NaN (GMT, G, 0, G->header->n_rows - 1, G->header->n_columns - C->q_pad[XHI] + 2, G->header->n_columns - 1);
	if (C->q_pad[YLO] > 2)	/* We extended the grid southwards so need to set the last rows on the bottom to NaN */
		surface_set_NaN (GMT, G, G->header->n_rows - C->q_pad[YLO] + 2, G->header->n_rows - 1, 0, G->header->n_columns - 1);
	if (C->q_pad[YHI] > 2) 	/* We extended the grid northwards so need to set the first rows on the top to NaN */
		surface_set_NaN (GMT, G, 0, C->q_pad[YHI] - 3, 0, G->header->n_columns - 1);
}

GMT_LOCAL int surface_load_constraints (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, int transform) {
	/* Deal with the constants or grids supplied via -L.  Note: Because we remove a
	 * best-fitting plane from the data, even a simple constant constraint will become
	 * a plane and thus must be represented on a grid. */
	unsigned int end, col, row;
	uint64_t node;
	char *limit[2] = {"Lower", "Upper"};
	double y_up;
	struct GMTAPI_CTRL *API = GMT->parent;

	GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Load any data constraint limit grids\n");

	/* Load lower/upper limits, verify range, deplane, and rescale */

	for (end = LO; end <= HI; end++) {
		if (C->set_limit[end] == NONE) continue;	/* Nothing desired */
		if (C->set_limit[end] < SURFACE) {	/* Got a constant level for this end */
			if ((C->Bound[end] = GMT_Duplicate_Data (API, GMT_IS_GRID, GMT_DUPLICATE_ALLOC, C->Grid)) == NULL) return (API->error);
			for (node = 0; node < C->mxmy; node++) C->Bound[end]->data[node] = (gmt_grdfloat)C->limit[end];
		}
		else {	/* Got a grid with a surface */
			if ((C->Bound[end] = GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, C->limit_file[end], NULL)) == NULL) return (API->error);	/* Get header only */
			if (!C->adjusted && (C->Bound[end]->header->n_columns != C->Grid->header->n_columns || C->Bound[end]->header->n_rows != C->Grid->header->n_rows)) {
				GMT_Report (API, GMT_MSG_ERROR, "%s limit file not of proper dimensions!\n", limit[end]);
				return (GMT_RUNTIME_ERROR);
			}
			if (GMT_Read_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_DATA_ONLY, NULL, C->limit_file[end], C->Bound[end]) == NULL) return (API->error);
			if (C->adjusted)	/* Must adjust padding and region and set new nodes to NaN */
				surface_enlarge_constraint_grid (GMT, C, C->Bound[end]);
		}
		if (transform) {	/* Remove best-fitting plane and normalize the bounding values */
			for (row = 0; row < C->Grid->header->n_rows; row++) {
				y_up = (double)(C->Grid->header->n_rows - row - 1);	/* Require y_up = 0 at south and positive toward north */
				node = row_col_to_node (row, 0, C->current_mx);
				for (col = 0; col < C->Grid->header->n_columns; col++, node++) {
					if (gmt_M_is_fnan (C->Bound[end]->data[node])) continue;
					C->Bound[end]->data[node] -= (gmt_grdfloat)evaluate_plane (C, col, y_up);	/* Remove plane */
					C->Bound[end]->data[node] *= (gmt_grdfloat)C->r_z_rms;				/* Normalize residuals */
				}
			}
		}
		C->constrained = true;	/* At least one of the limits will be constrained */
		if (C->Bh == NULL) C->Bh = C->Bound[end]->header;	/* Just pick either one of them */
	}

	return (0);
}

GMT_LOCAL int surface_write_grid (struct GMT_CTRL *GMT, struct SURFACE_CTRL *Ctrl, struct SURFACE_INFO *C, char *grdfile) {
	/* Write output grid to file */
	uint64_t node;
	openmp_int row, col;
	int err, end;
	char *limit[2] = {"lower", "upper"};
	gmt_grdfloat *u = C->Grid->data;

	if (!Ctrl->Q.active && Ctrl->Q.adjusted) {	/* Probably need to shrink region to the desired one by increasing the pads */
		int del_pad[4] = {0, 0, 0, 0}, k, n = 0;
		struct GMT_GRID_HEADER_HIDDEN *HH = gmt_get_H_hidden (C->Grid->header);
		/* Determine the shifts inwards for each side */
		del_pad[XLO] = irint ((C->wesn_orig[XLO] - C->Grid->header->wesn[XLO]) * HH->r_inc[GMT_X]);
		del_pad[XHI] = irint ((C->Grid->header->wesn[XHI] - C->wesn_orig[XHI]) * HH->r_inc[GMT_X]);
		del_pad[YLO] = irint ((C->wesn_orig[YLO] - C->Grid->header->wesn[YLO]) * HH->r_inc[GMT_Y]);
		del_pad[YHI] = irint ((C->Grid->header->wesn[YHI] - C->wesn_orig[YHI]) * HH->r_inc[GMT_Y]);
		for (k = 0; k < 4; k++) n += abs (del_pad[k]);	/* See if any is needed */
		if (n) {	/* Yep, must update pad and all meta data for this grid first */
			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Increase pad by %d %d %d %d\n", del_pad[XLO], del_pad[XHI], del_pad[YLO], del_pad[YHI]);
			for (k = 0; k < 4; k++) C->Grid->header->pad[k] += del_pad[k];	/* Increase pad to shrink region */
			gmt_M_memcpy (C->Grid->header->wesn, C->wesn_orig, 4U, double);	/* Reset -R to what was requested */
			gmt_set_grddim (GMT, C->Grid->header);	/* Update dimensions given the change of pad */
		}
	}

	GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Prepare final output grid [stride = %d]\n", C->current_stride);

	strcpy (C->Grid->header->title, "Data gridded with continuous surface splines in tension");

	if (GMT->common.R.registration == GMT_GRID_PIXEL_REG) {	/* Pixel registration request. Reset region to the original extent */
		gmt_M_memcpy (C->Grid->header->wesn, C->wesn_orig, 4, double);
		C->Grid->header->registration = GMT->common.R.registration;
		/* Must reduce both n_columns,n_rows by 1 and make the easternmost column and northernmost row part of the grid pad */
		C->Grid->header->n_columns--;	C->n_columns--;
		C->Grid->header->n_rows--;	C->n_rows--;
		C->Grid->header->pad[XHI]++;	/* Presumably increase pad from 2 to 3 */
		C->Grid->header->pad[YHI]++;	/* Presumably increase pad from 2 to 3 */
		gmt_set_grddim (GMT, C->Grid->header);	/* Reset all integer dimensions and xy_off */
	}
	if (C->constrained) {	/* Must check that we don't exceed any imposed limits.  */
		/* Reload the constraints, but this time do not transform the data */
		if ((err = surface_load_constraints (GMT, C, false)) != 0) return (err);

		gmt_M_grd_loop (GMT, C->Grid, row, col, node) {	/* Make sure we clip to the specified bounds */
			if (C->set_limit[LO] && !gmt_M_is_fnan (C->Bound[LO]->data[node]) && u[node] < C->Bound[LO]->data[node]) u[node] = C->Bound[LO]->data[node];
			if (C->set_limit[HI] && !gmt_M_is_fnan (C->Bound[HI]->data[node]) && u[node] > C->Bound[HI]->data[node]) u[node] = C->Bound[HI]->data[node];
		}
		/* Free any bounding surfaces */
		for (end = LO; end <= HI; end++) {
			if ((C->set_limit[end] > NONE && C->set_limit[end] < SURFACE) && GMT_Destroy_Data (GMT->parent, &C->Bound[end]) != GMT_NOERROR) {
				GMT_Report (GMT->parent, GMT_MSG_ERROR, "Failed to free %s boundary\n", limit[end]);
			}
		}
	}
	if (C->periodic) {	/* Ensure exact periodicity at E-W boundaries */
		for (row = 0; row < (openmp_int)C->current_ny; row++) {
			node = row_col_to_node (row, 0, C->current_mx);
			u[node] = u[node+C->current_nx-1] = (gmt_grdfloat)(0.5 * (u[node] + u[node+C->current_nx-1]));	/* Set these to the same as their average */
		}
	}
	/* Time to write our final grid */
	if (GMT_Write_Data (GMT->parent, GMT_IS_GRID, GMT_IS_FILE, GMT_IS_SURFACE, GMT_CONTAINER_AND_DATA, NULL, grdfile, C->Grid) != GMT_NOERROR)
		return (GMT->parent->error);

	return (0);
}

GMT_LOCAL void surface_set_BCs (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, gmt_grdfloat *u) {
	/* Fill in auxiliary boundary rows and columns; see equations (A-8,9,10) in the reference */
	uint64_t n, n_s, n_n, n_w, n_e;	/* Node variables */
	int col, row, *d_n = C->offset;	/* Relative changes in node index from present node n */
	double x_0_const = 4.0 * (1.0 - C->boundary_tension) / (2.0 - C->boundary_tension);
	double x_1_const = (3 * C->boundary_tension - 2.0) / (2.0 - C->boundary_tension);
	double y_denom = 2 * C->alpha * (1.0 - C->boundary_tension) + C->boundary_tension;
	double y_0_const = 4 * C->alpha * (1.0 - C->boundary_tension) / y_denom;
	double y_1_const = (C->boundary_tension - 2 * C->alpha * (1.0 - C->boundary_tension) ) / y_denom;

	GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Apply all boundary conditions [stride = %d]\n", C->current_stride);

	/* First set (1-T)d2[]/dn2 + Td[]/dn = 0 along edges */

	for (col = 0, n_s = C->node_sw_corner, n_n = C->node_nw_corner; col < C->current_nx; col++, n_s++, n_n++) {	/* set BC1 along south and north side */
		u[n_s+d_n[S1]] = (gmt_grdfloat)(y_0_const * u[n_s] + y_1_const * u[n_s+d_n[N1]]);	/* South: u_{0,-1} = 2 * u_{0,0} - u_{0,+1} */
		u[n_n+d_n[N1]] = (gmt_grdfloat)(y_0_const * u[n_n] + y_1_const * u[n_n+d_n[S1]]);	/* North: u_{0,+1} = 2 * u_{0,0} - u_{0,-1} */
	}
	if (C->periodic) {	/* Set periodic boundary conditions in longitude at west and east boundaries */
		for (row = 0, n_w = C->node_nw_corner, n_e = C->node_ne_corner; row < C->current_ny; row++, n_w += C->current_mx, n_e += C->current_mx) {
			u[n_w+d_n[W1]] = u[n_e+d_n[W1]];	/* West side */
			u[n_e+d_n[E1]] = u[n_w+d_n[E1]];	/* East side */
			u[n_e] = u[n_w] = 0.5f * (u[n_e] + u[n_w]);	/* Set to average of east and west */
		}
	}
	else {	/* Regular natural BC */
		for (row = 0, n_w = C->node_nw_corner, n_e = C->node_ne_corner; row < C->current_ny; row++, n_w += C->current_mx, n_e += C->current_mx) {
			/* West: u_{-10} = 2 * u_{00} - u_{10}  */
			u[n_w+d_n[W1]] = (gmt_grdfloat)(x_1_const * u[n_w+d_n[E1]] + x_0_const * u[n_w]);
			/* East: u_{10} = 2 * u_{00} - u_{-10}  */
			u[n_e+d_n[E1]] = (gmt_grdfloat)(x_1_const * u[n_e+d_n[W1]] + x_0_const * u[n_e]);
		}
	}

	/* Now set d2[]/dxdy = 0 at each of the 4 corners */

	n = C->node_sw_corner;	/* Just use shorthand in each expression */
	u[n+d_n[SW]]  = u[n+d_n[SE]] + u[n+d_n[NW]] - u[n+d_n[NE]];
	n = C->node_nw_corner;
	u[n+d_n[NW]]  = u[n+d_n[NE]] + u[n+d_n[SW]] - u[n+d_n[SE]];
	n = C->node_se_corner;
	u[n+d_n[SE]]  = u[n+d_n[SW]] + u[n+d_n[NE]] - u[n+d_n[NW]];
	n = C->node_ne_corner;
	u[n+d_n[NE]]  = u[n+d_n[NW]] + u[n+d_n[SE]] - u[n+d_n[SW]];

	/* Now set dC/dn = 0 at each edge */

	for (col = 0, n_s = C->node_sw_corner, n_n = C->node_nw_corner; col < C->current_nx; col++, n_s++, n_n++) {	/* set BC2 along south and north side */
		/* South side */
		u[n_s+d_n[S2]] = (gmt_grdfloat)(u[n_s+d_n[N2]] + C->eps_m2 * (u[n_s+d_n[NW]] + u[n_s+d_n[NE]]
			- u[n_s+d_n[SW]] - u[n_s+d_n[SE]]) + C->two_plus_em2 * (u[n_s+d_n[S1]] - u[n_s+d_n[N1]]));
		/* North side */
		u[n_n+d_n[N2]] = (gmt_grdfloat)(u[n_n+d_n[S2]] + C->eps_m2 * (u[n_n+d_n[SW]] + u[n_n+d_n[SE]]
			- u[n_n+d_n[NW]] - u[n_n+d_n[NE]]) + C->two_plus_em2 * (u[n_n+d_n[N1]] - u[n_n+d_n[S1]]));
	}

	for (row = 0, n_w = C->node_nw_corner, n_e = C->node_ne_corner; row < C->current_ny; row++, n_w += C->current_mx, n_e += C->current_mx) {	/* set BC2 along west and east side */
		if (C->periodic) {	/* Set periodic boundary conditions in longitude */
			u[n_w+d_n[W2]] = u[n_e+d_n[W2]];	/* West side */
			u[n_e+d_n[E2]] = u[n_w+d_n[E2]];	/* East side */
		}
		else {	/* Natural BCs */
			/* West side */
			u[n_w+d_n[W2]] = (gmt_grdfloat)(u[n_w+d_n[E2]] + C->eps_p2 * (u[n_w+d_n[NE]] + u[n_w+d_n[SE]]
				- u[n_w+d_n[NW]] - u[n_w+d_n[SW]]) + C->two_plus_ep2 * (u[n_w+d_n[W1]] - u[n_w+d_n[E1]]));
			/* East side */
			u[n_e+d_n[E2]] = (gmt_grdfloat)(u[n_e+d_n[W2]] + C->eps_p2 * (u[n_e+d_n[NW]] + u[n_e+d_n[SW]]
				- u[n_e+d_n[NE]] - u[n_e+d_n[SE]]) + C->two_plus_ep2 * (u[n_e+d_n[E1]] - u[n_e+d_n[W1]]));
		}
	}
}

GMT_LOCAL uint64_t surface_iterate (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, int mode) {
	/* Main finite difference solver */
	uint64_t node, briggs_index, iteration_count = 0, node_final = 0;
	unsigned int set, quadrant, current_max_iterations = C->max_iterations * C->current_stride;
	int col, row, k, *d_node = C->offset;	/* Relative changes in node index from present node */
	unsigned char *status = C->status;	/* Quadrant or status information for each node */
	char *mode_name[2] = {"node", "data"};
	bool finished;
	double current_limit = C->converge_limit / C->current_stride;
	double u_change, max_u_change, max_z_change, sum_bk_uk, u_00;
	gmt_grdfloat *b = NULL;
	gmt_grdfloat *u_new = C->Grid->data, *u_old = C->Grid->data;

	GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Starting iterations, mode = %s Max iterations = %d [stride = %d]\n", mode_name[mode], current_max_iterations, C->current_stride);

	sprintf (C->format, "%%4ld\t%%c\t%%8" PRIu64 "\t%s\t%s\t%%10" PRIu64 "\n", GMT->current.setting.format_float_out, GMT->current.setting.format_float_out);
	if (C->logging) fprintf (C->fp_log, "%c Grid size = %d Mode = %c Convergence limit = %g -Z%d\n",
		GMT->current.setting.io_seg_marker[GMT_OUT], C->current_stride, C->mode_type[mode], current_limit, C->current_stride);

	/* We need to do an even number of iterations so that the final result for this iteration resides in C->Grid->data */
	do {

		surface_set_BCs (GMT, C, u_old);	/* Set the boundary rows and columns */

		briggs_index = 0;	/* Reset the Briggs constraint table index  */
		max_u_change = -1.0;	/* Ensure max_u_change is < 0 for starters */

		/* Now loop over all interior data nodes */
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Iteration %d\n", iteration_count);

		for (row = 0; row < C->current_ny; row++) {	/* Loop over rows */
			node = C->node_nw_corner + row * C->current_mx;	/* Node at left side of this row */
			if (C->constrained) node_final = gmt_M_ijp (C->Bh, C->current_stride * row, 0);
			for (col = 0; col < C->current_nx; col++, node++, node_final += C->current_stride) {	/* Loop over all columns */
				if (status[node] == SURFACE_IS_CONSTRAINED) {	/* Data constraint fell exactly on the node, keep it as is */
					continue;
				}

				/* Here we must estimate a solution via equations (A-4) [SURFACE_UNCONSTRAINED] or (A-7) [SURFACE_CONSTRAINED] */
				u_00 = 0.0;	/* Start with zero, build updated solution for central node */
				set = (status[node] == SURFACE_IS_UNCONSTRAINED) ? SURFACE_UNCONSTRAINED : SURFACE_CONSTRAINED;	/* Index to C->coeff set to use */
				for (k = 0; k < 12; k++) {	/* This is either equation (A-4) or the corresponding part of (A-7), depending on the value of set */
					u_00 += (u_old[node+d_node[k]] * C->coeff[set][k]);
				}
				if (set == SURFACE_CONSTRAINED) {	/* Solution is (A-7) and modifications depend on which quadrant the point lies in */
					b = C->Briggs[briggs_index].b;		/* Shorthand to this node's Briggs b-array */
					quadrant = status[node];		/* Which quadrant did the point fall in? */
					for (k = 0, sum_bk_uk = 0.0; k < 4; k++) {	/* Sum over b[k]*u[k] for nodes A-D in Fig A-1 */
						sum_bk_uk += b[k] * u_old[node+d_node[C->p[quadrant][k]]];
					}
					u_00 = (u_00 + C->a0_const_2 * (sum_bk_uk + b[4])) * b[5];	/* Add point E in Fig A-1 to sum_bk_uk and normalize */
					briggs_index++;	/* Got to next sequential Briggs array index */
				}
				/* We now apply the over-relaxation: */
				u_00 = u_old[node] * C->relax_old + u_00 * C->relax_new;
				if (C->constrained) {	/* Must check that we don't exceed any imposed limits.  */
					/* Must use final spacing node index to access the Bound grids */
					if (C->set_limit[LO] && !gmt_M_is_fnan (C->Bound[LO]->data[node_final]) && u_00 < C->Bound[LO]->data[node_final])
						u_00 = C->Bound[LO]->data[node_final];
					else if (C->set_limit[HI] && !gmt_M_is_fnan (C->Bound[HI]->data[node_final]) && u_00 > C->Bound[HI]->data[node_final])
						u_00 = C->Bound[HI]->data[node_final];
				}
				u_change = fabs (u_00 - u_old[node]);		/* Change in node value between iterations */
				u_new[node] = (gmt_grdfloat)u_00;			/* Our updated estimate at this node */
				if (u_change > max_u_change) max_u_change = u_change;	/* Keep track of max u_change across all nodes */
			}	/* End of loop over columns */
		}	/* End of loop over rows [and possibly threads via OpenMP] */

		iteration_count++;	C->total_iterations++;	/* Update iteration counts for this stride and for total */
		max_z_change = max_u_change * C->z_rms;		/* Scale max_u_change back into original z units -> max_z_change */
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, C->format,
			C->current_stride, C->mode_type[mode], iteration_count, max_z_change, current_limit, C->total_iterations);
		if (C->logging) fprintf (C->fp_log, "%d\t%c\t%" PRIu64 "\t%.8g\t%.8g\t%" PRIu64 "\n", C->current_stride, C->mode_type[mode], iteration_count, max_z_change, current_limit, C->total_iterations);
		finished = (max_z_change <= current_limit || iteration_count >= current_max_iterations);

	} while (!finished);

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, C->format,
		C->current_stride, C->mode_type[mode], iteration_count, max_z_change, current_limit, C->total_iterations);

	return (iteration_count);
}

GMT_LOCAL void surface_check_errors (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* Compute misfits at original data locations,  This is only done at the
	 * final grid resolution, hence current_stride == 1. */

	uint64_t node, k;
	openmp_int row, col;
	int *d_node = C->offset;
	unsigned char *status = C->status;

	double x0, y0, dx, dy, mean_error = 0.0, mean_squared_error = 0.0, z_est, z_err, curvature, c;
	double du_dx, du_dy, d2u_dx2, d2u_dxdy, d2u_dy2, d3u_dx3, d3u_dx2dy, d3u_dxdy2, d3u_dy3;

	gmt_grdfloat *u = C->Grid->data;
	struct GMT_GRID_HEADER *h = C->Grid->header;
	struct GMT_GRID_HEADER_HIDDEN *HH = gmt_get_H_hidden (h);

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Compute rms misfit and curvature.\n");

	surface_set_BCs (GMT, C, u);	/* First update the boundary values */

	/* Estimate solution at all data constraints using 3rd order Taylor expansion from nearest node */

	for (k = 0; k < C->npoints; k++) {
		row = (openmp_int)index_to_row (C->data[k].index, C->n_columns);
		col = (openmp_int)index_to_col (C->data[k].index, C->n_columns);
		node = row_col_to_node (row, col, C->mx);
	 	if (status[node] == SURFACE_IS_CONSTRAINED) continue;	/* Since misfit by definition is zero so no point adding it */
		/* Get coordinates of this node */
		x0 = col_to_x (col, h->wesn[XLO], h->wesn[XHI], h->inc[GMT_X], h->n_columns);
		y0 = row_to_y (row, h->wesn[YLO], h->wesn[YHI], h->inc[GMT_Y], h->n_rows);
		/* Get dx,dy of data point away from this node */
		dx = x_to_fcol (C->data[k].x, x0, HH->r_inc[GMT_X]);
		dy = y_to_frow (C->data[k].y, y0, HH->r_inc[GMT_Y]);

	 	du_dx = 0.5 * (u[node+d_node[E1]] - u[node+d_node[W1]]);
	 	du_dy = 0.5 * (u[node+d_node[N1]] - u[node+d_node[S1]]);
	 	d2u_dx2 = u[node+d_node[E1]] + u[node+d_node[W1]] - 2 * u[node];
	 	d2u_dy2 = u[node+d_node[N1]] + u[node+d_node[S1]] - 2 * u[node];
	 	d2u_dxdy = 0.25 * (u[node+d_node[NE]] - u[node+d_node[NW]]
	 		- u[node+d_node[SE]] + u[node+d_node[SW]]);
	 	d3u_dx3 = 0.5 * (u[node+d_node[E2]] - 2 * u[node+d_node[E1]]
	 		+ 2 * u[node+d_node[W1]] - u[node+d_node[W2]]);
	 	d3u_dy3 = 0.5 * (u[node+d_node[N2]] - 2 * u[node+d_node[N1]]
	 		+ 2 * u[node+d_node[S1]] - u[node+d_node[S2]]);
	 	d3u_dx2dy = 0.5 * ((u[node+d_node[NE]] + u[node+d_node[NW]] - 2 * u[node+d_node[N1]])
	 		- (u[node+d_node[SE]] + u[node+d_node[SW]] - 2 * u[node+d_node[S1]]));
	 	d3u_dxdy2 = 0.5 * ((u[node+d_node[NE]] + u[node+d_node[SE]] - 2 * u[node+d_node[E1]])
	 		- (u[node+d_node[NW]] + u[node+d_node[SW]] - 2 * u[node+d_node[W1]]));

	 	/* Compute the 3rd order Taylor approximation from current node */

	 	z_est = u[node] + dx * (du_dx +  dx * ((0.5 * d2u_dx2) + dx * (d3u_dx3 / 6.0)))
			+ dy * (du_dy +  dy * ((0.5 * d2u_dy2) + dy * (d3u_dy3 / 6.0)))
	 		+ dx * dy * (d2u_dxdy) + (0.5 * dx * d3u_dx2dy) + (0.5 * dy * d3u_dxdy2);

	 	z_err = z_est - C->data[k].z;	/* Misfit between surface estimate and observation */
	 	mean_error += z_err;
	 	mean_squared_error += (z_err * z_err);
	 }
	 mean_error /= C->npoints;
	 mean_squared_error = sqrt (mean_squared_error / C->npoints);

	/* Compute the total curvature of the grid */

	curvature = 0.0;
	gmt_M_grd_loop (GMT, C->Grid, row, col, node) {
 		c = u[node+d_node[E1]] + u[node+d_node[W1]] + u[node+d_node[N1]] + u[node+d_node[S1]] - 4.0 * u[node+d_node[E1]];
		curvature += (c * c);
	}

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Fit info: N data points  N nodes\tmean error\trms error\tcurvature\n");
	sprintf (C->format,"\t%%8ld\t%%8ld\t%s\t%s\t%s\n", GMT->current.setting.format_float_out, GMT->current.setting.format_float_out, GMT->current.setting.format_float_out);
	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, C->format, C->npoints, C->nxny, mean_error, mean_squared_error, curvature);
}

GMT_LOCAL void surface_remove_planar_trend (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* Fit LS plane and remove trend from our (x,y,z) input data; we add trend to grid before output.
	 * Note: Here, x and y are first converted to fractional grid spacings from 0 to {n_columns,n_rows}-1.
	 * Hence the same scheme is used by replace_planar trend (i.e., just use row,col as coordinates).
	 * Note: The plane is fit to the original data z-values before normalizing by rms. */
	uint64_t k;
	double a, b, c, d, xx, y_up, zz, sx, sy, sz, sxx, sxy, sxz, syy, syz;
	struct GMT_GRID_HEADER *h = C->Grid->header;
	struct GMT_GRID_HEADER_HIDDEN *HH = gmt_get_H_hidden (h);

	sx = sy = sz = sxx = sxy = sxz = syy = syz = 0.0;

	for (k = 0; k < C->npoints; k++) {	/* Sum up normal equation terms */
		xx = x_to_fcol (C->data[k].x, h->wesn[XLO], HH->r_inc[GMT_X]);	/* Distance from west to this point */
		y_up = y_to_frow (C->data[k].y, h->wesn[YLO], HH->r_inc[GMT_Y]);	/* Distance from south to this point */
		zz = C->data[k].z;
		sx += xx;		sy += y_up;		sz += zz;		sxx += (xx * xx);
		sxy += (xx * y_up);	sxz += (xx * zz);	syy += (y_up * y_up);	syz += (y_up * zz);
	}

	d = C->npoints*sxx*syy + 2*sx*sy*sxy - C->npoints*sxy*sxy - sx*sx*syy - sy*sy*sxx;

	if (d == 0.0) {	/* When denominator is zero we have a horizontal plane */
		C->plane_icept = C->plane_sx = C->plane_sy = 0.0;
		return;
	}

	a = sz*sxx*syy + sx*sxy*syz + sy*sxy*sxz - sz*sxy*sxy - sx*sxz*syy - sy*syz*sxx;
	b = C->npoints*sxz*syy + sz*sy*sxy + sy*sx*syz - C->npoints*sxy*syz - sz*sx*syy - sy*sy*sxz;
	c = C->npoints*sxx*syz + sx*sy*sxz + sz*sx*sxy - C->npoints*sxy*sxz - sx*sx*syz - sz*sy*sxx;

	C->plane_icept = a / d;
	C->plane_sx    = b / d;
	C->plane_sy    = c / d;
	if (C->periodic) C->plane_sx = 0.0;	/* Cannot have x-trend for periodic geographic data */

	for (k = 0; k < C->npoints; k++) {	/* Now remove this plane from the data constraints */
		xx = x_to_fcol (C->data[k].x, h->wesn[XLO], HH->r_inc[GMT_X]);
		y_up = y_to_frow (C->data[k].y, h->wesn[YLO], HH->r_inc[GMT_Y]);
		C->data[k].z -= (gmt_grdfloat)evaluate_plane(C, xx, y_up);
	}

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Plane fit z = %g + (%g * col) + (%g * row)\n", C->plane_icept, C->plane_sx, C->plane_sy);
}

GMT_LOCAL void surface_restore_planar_trend (struct SURFACE_INFO *C) {
	/* Scale grid back up by the data rms and restore the least-square plane.
	 * Note: In determining the plane and in evaluating it, remember that the
	 * x and y coordinates needed are not data coordinates but fractional col
	 * and row distances from an origin at the lower left (southwest corner).
	 * This means the y-values are positive up and increase in the opposite
	 * direction than how rows increase. Hence the use of y_up below. */
	unsigned int row, col;
	uint64_t node;
	gmt_grdfloat *u = C->Grid->data;
	double y_up;	/* Measure y up from south in fractional rows */

	for (row = 0; row < C->Grid->header->n_rows; row++) {
		y_up = (double)(C->Grid->header->n_rows - row - 1);	/* # of rows from south (where y_up = 0) to this node */
		node = row_col_to_node (row, 0, C->current_mx);	/* Node index at left end of interior row */
		for (col = 0; col < C->Grid->header->n_columns; col++, node++)	/* March across this row */
		 	u[node] = (gmt_grdfloat)((u[node] * C->z_rms) + (evaluate_plane (C, col, y_up)));
	}
}

GMT_LOCAL void surface_throw_away_unusables (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* We eliminate data which will become unusable on the final iteration, when current_stride = 1.
	   It assumes current_stride = 1 and that surface_set_grid_parameters has been called.
	   We sort, mark redundant data as SURFACE_OUTSIDE, and sort again, chopping off the excess.
	*/

	uint64_t last_index = UINTMAX_MAX, n_outside = 0, k, last_k;

	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Eliminate data points that are not nearest a node.\n");

	/* Sort the data  */

	QSORT_R (C->data, C->npoints, sizeof (struct SURFACE_DATA), surface_compare_points, &(C->info));

	/* If more than one datum is indexed to the same node, only the first should be kept.
	   Mark the additional ones as SURFACE_OUTSIDE.
	*/
	for (k = 0; k < C->npoints; k++) {
		if (C->data[k].index == last_index) {	/* Same node but further away than our guy */
			C->data[k].index = SURFACE_OUTSIDE;
			n_outside++;
			GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Skipping unusable point at (%.16lg %.16lg %.16lg) as (%.16lg %.16lg %.16lg) is closer to node %" PRIu64 "\n",
					C->data[k].x, C->data[k].y, C->data[k].z, C->data[last_k].x, C->data[last_k].y, C->data[last_k].z, last_index);
		}
		else {	/* New index, just update last_index */
			last_index = C->data[k].index;
			last_k = k;
		}
	}

	if (n_outside) {	/* Sort again; this time the SURFACE_OUTSIDE points will be sorted to end of the array */
		QSORT_R (C->data, C->npoints, sizeof (struct SURFACE_DATA), surface_compare_points, &(C->info));
		C->npoints -= n_outside;	/* Effectively chopping off the eliminated points */
		C->data = gmt_M_memory (GMT, C->data, C->npoints, struct SURFACE_DATA);	/* Adjust memory accordingly */
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "%" PRIu64 " unusable points were supplied; these will be ignored.\n", n_outside);
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "You should have pre-processed the data with block-mean, -median, or -mode.\n");
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "Check that previous processing steps write results with enough decimals.\n");
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "Possibly some data were half-way between nodes and subject to IEEE 754 rounding.\n");
	}
}

GMT_LOCAL int surface_rescale_z_values (struct GMT_CTRL *GMT, struct SURFACE_INFO *C) {
	/* Find and normalize data by their rms value */
	uint64_t k;
	double ssz = 0.0;

	for (k = 0; k < C->npoints; k++) ssz += (C->data[k].z * C->data[k].z);
	C->z_rms = sqrt (ssz / C->npoints);
	GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Normalize detrended data constraints by z rms = %g\n", C->z_rms);

	if (C->z_rms < GMT_CONV8_LIMIT) {
		GMT_Report (GMT->parent, GMT_MSG_WARNING, "Input data lie exactly on a plane.\n");
		C->r_z_rms = C->z_rms = 1.0;
		return (1);	/* Flag to tell the main to just write out the plane */
	}
	else
		C->r_z_rms = 1.0 / C->z_rms;

	for (k = 0; k < C->npoints; k++) C->data[k].z *= (gmt_grdfloat)C->r_z_rms;

	if (C->converge_limit == 0.0 || C->converge_mode == BY_PERCENT) {	/* Set default values for convergence criteria */
		unsigned int ppm;
		double limit = (C->converge_mode == BY_PERCENT) ? C->converge_limit : SURFACE_CONV_LIMIT;
		ppm = urint (limit / 1.0e-6);
		C->converge_limit = limit * C->z_rms; /* i.e., 100 ppm of L2 scale */
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Select default convergence limit of %g (%u ppm of L2 scale)\n", C->converge_limit, ppm);
	}
	return (0);
}

GMT_LOCAL unsigned int surface_suggest_sizes (struct GMT_CTRL *GMT, struct SURFACE_CTRL *Ctrl, struct GMT_GRID *G, unsigned int factors[], unsigned int n_columns, unsigned int n_rows, bool pixel) {
	/* Calls gmt_optimal_dim_for_surface to determine if there are
	 * better choices for n_columns, n_rows that might speed up calculations
	 * by having many more common factors.
	 *
	 * W. H. F. Smith, 26 Feb 1992.  */

	unsigned int k;
	unsigned int n_sug = 0;	/* Number of suggestions found */
	struct GMT_SURFACE_SUGGESTION *sug = NULL;

	n_sug = gmt_optimal_dim_for_surface (GMT, factors, n_columns, n_rows, &sug);

	if (n_sug) {	/* We did find some suggestions, report them (up to the first 10 suggestions) */
		char region[GMT_LEN128] = {""}, buffer[GMT_LEN128] = {""};
		bool lat_bad = false;
		unsigned int m, save_range = GMT->current.io.geo.range;
		double w, e, s, n;
		GMT->current.io.geo.range = GMT_IS_GIVEN_RANGE;		/* Override this setting explicitly */
		for (k = 0; k < n_sug && k < 10; k++) {
			m = sug[k].n_columns - (G->header->n_columns - 1);	/* Additional nodes needed in x to give more factors */
			w = G->header->wesn[XLO] - (m/2)*G->header->inc[GMT_X];	/* Potential revised w/e extent */
			e = G->header->wesn[XHI] + (m/2)*G->header->inc[GMT_X];
			if (m%2) e += G->header->inc[GMT_X];
			m = sug[k].n_rows - (G->header->n_rows - 1);	/* Additional nodes needed in y to give more factors */
			s = G->header->wesn[YLO] - (m/2)*G->header->inc[GMT_Y];	/* Potential revised s/n extent */
			n = G->header->wesn[YHI] + (m/2)*G->header->inc[GMT_Y];
			if (!lat_bad && gmt_M_y_is_lat (GMT, GMT_IN) && (s < -90.0 || n > 90.0))
				lat_bad = true;
			if (m%2) n += G->header->inc[GMT_Y];
			if (pixel) {	/* Since we already added 1/2 pixel we need to undo that here so the report matches original phase */
				w -= G->header->inc[GMT_X] / 2.0;	e -= G->header->inc[GMT_X] / 2.0;
				s -= G->header->inc[GMT_Y] / 2.0;	n -= G->header->inc[GMT_Y] / 2.0;
			}
			gmt_ascii_format_col (GMT, buffer, w, GMT_OUT, GMT_X);
			sprintf (region, "-R%s/", buffer);
			gmt_ascii_format_col (GMT, buffer, e, GMT_OUT, GMT_X);
			strcat (region, buffer);	strcat (region, "/");
			gmt_ascii_format_col (GMT, buffer, s, GMT_OUT, GMT_Y);
			strcat (region, buffer);	strcat (region, "/");
			gmt_ascii_format_col (GMT, buffer, n, GMT_OUT, GMT_Y);
			strcat (region, buffer);
			if (Ctrl->Q.active == false) {	/* We just want to know the best w/e/s/n */
				Ctrl->Q.wesn[XLO] = w;	Ctrl->Q.wesn[XHI] = e;	Ctrl->Q.wesn[YLO] = s; Ctrl->Q.wesn[YHI] = n;
				GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Internally speed up convergence by using the larger region %s (go from %d x %d to optimal %d x %d, with speedup-factor %.8lg)\n",
					region, n_columns, n_rows, sug[k].n_columns, sug[k].n_rows, sug[k].factor);
				gmt_M_free (GMT, sug);
				return (1);
			}
			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Hint: Choosing %s [n_columns = %d, n_rows = %d] might cut run time by a factor of %.8g\n",
				region, sug[k].n_columns, sug[k].n_rows, sug[k].factor);
		}
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Hint: After completion you can recover the desired region via gmt grdcut\n");
		if (lat_bad) {
			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Note: One or more of the suggested south/north bounds exceed the allowable range [-90/90]\n");
			GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "A workaround is to use -fx to only consider x as geographic longitudes\n");
		}
		gmt_M_free (GMT, sug);
		GMT->current.io.geo.range = save_range;
	}
	else
		GMT_Report (GMT->parent, GMT_MSG_INFORMATION, "Cannot suggest any n_columns,n_rows better than your current -R -I settings.\n");
	return (n_sug);
}

GMT_LOCAL void surface_init_parameters (struct SURFACE_INFO *C, struct SURFACE_CTRL *Ctrl) {
	/* Place program options into the surface struct.  This was done this way
	 * since surface.c relied heavily on global variables which are a no-no
	 * in GMT5.  The simplest solution was to collect all those variables into
	 * a single structure and pass a pointer to that structure to functions.
	 */
	if (Ctrl->S.active) {	/* Gave a search radius; adjust if minutes or seconds where specified */
		if (Ctrl->S.unit == 'm') Ctrl->S.radius /= 60.0;
		if (Ctrl->S.unit == 's') Ctrl->S.radius /= 3600.0;
	}
	C->radius		= Ctrl->S.radius;
	C->relax_new		= Ctrl->Z.value;
	C->relax_old		= 1.0 - C->relax_new;
	C->max_iterations	= Ctrl->N.value;
	C->radius		= Ctrl->S.radius;
	C->limit_file[LO]	= Ctrl->L.file[LO];
	C->limit_file[HI]	= Ctrl->L.file[HI];
	C->set_limit[LO]	= Ctrl->L.mode[LO];
	C->set_limit[HI]	= Ctrl->L.mode[HI];
	C->limit[LO]		= Ctrl->L.limit[LO];
	C->limit[HI]		= Ctrl->L.limit[HI];
	C->adjusted			= Ctrl->Q.adjusted;
	C->boundary_tension	= Ctrl->T.b_tension;
	C->interior_tension	= Ctrl->T.i_tension;
	C->alpha		= Ctrl->A.value;
	C->converge_limit	= Ctrl->C.value;
	C->converge_mode	= Ctrl->C.mode;
	C->n_alloc		= GMT_INITIAL_MEM_ROW_ALLOC;
	C->z_rms		= 1.0;
	C->r_z_rms		= 1.0;
	C->mode_type[0]		= 'I';	/* I means exclude data points when iterating */
	C->mode_type[1]		= 'D';	/* D means include data points when iterating */
	C->n_columns			= C->Grid->header->n_columns;
	C->n_rows			= C->Grid->header->n_rows;
	C->nxny			= C->Grid->header->nm;
	C->mx			= C->Grid->header->mx;
	C->my			= C->Grid->header->my;
	C->mxmy			= C->Grid->header->size;
	gmt_M_memcpy (C->p, p, 20, unsigned int);

	gmt_M_memcpy (C->info.wesn, C->Grid->header->wesn, 4, double);
}

GMT_LOCAL double surface_find_closest_point (double *x, double *y, double *z, uint64_t k, double x0, double y0, double half_dx, double half_dy, double *xx, double *yy, double *zz) {
	/* Find the point (xx,yy) on the line from (x[k-1],y[k-1]) to (x[k], y[k]) that is closest to (x0,y0).  If (xx,yy)
	 * is outside the end of the line segment or outside the bin then we return r = DBL_MAX */
	double dx, dy, a, r= DBL_MAX;	/* Initialize distance from (x0,y0) to nearest point (xx,yy) measured orthogonally onto break line */
	uint64_t km1 = k - 1;
	dx = x[k] - x[km1];	dy = y[k] - y[km1];
	if (gmt_M_is_zero (dx)) {	/* Break line is vertical */
		if ((y[k] <= y0 && y[km1] > y0) || (y[km1] <= y0 && y[k] > y0)) {	/* Nearest point is in same bin */
			*xx = x[k];	*yy = y0;
			r = fabs (*xx - x0);
			*zz = z[km1] + (z[k] - z[km1]) * (*yy - y[km1]) / dy;
		}
	}
	else if (gmt_M_is_zero (dy)) {	/* Break line is horizontal */
		if ((x[k] <= x0 && x[km1] > x0) || (x[km1] <= x0 && x[k] > x0)) {	/* Nearest point in same bin */
			*xx = x0;	*yy = y[k];
			r = fabs (*yy - y0);
			*zz = z[km1] + (z[k] - z[km1]) * (*xx - x[km1]) / dx;
		}
	}
	else {	/* General case.  Nearest orthogonal point may or may not be in bin, in which case r > r_prev */
		a = dy / dx;	/* Slope of line */
		*xx = (y0 - y[km1] + a * x[km1] + x0 / a) / (a + 1.0/a);
		*yy = a * (*xx - x[k]) + y[k];
		if ((x[k] <= *xx && x[km1] > *xx) || (x[km1] <= *xx && x[k] > *xx)) {	/* Orthonormal point found between the end points of line */
			if (fabs (*xx-x0) < half_dx && fabs (*yy-y0) < half_dy) {	/* Yes, within this bin */
				r = hypot (*xx - x0, *yy - y0);
				*zz = z[km1] + (z[k] - z[km1]) * (*xx - x[km1]) / dx;
			}
		}
	}
	return r;
}

GMT_LOCAL void surface_interpolate_add_breakline (struct GMT_CTRL *GMT, struct SURFACE_INFO *C, struct GMT_DATATABLE *T, char *file, bool fix_z, double z_level) {
	int srow, scol;
	uint64_t new_n = 0, n_int = 0, nb = 0;
	uint64_t k = 0, n, kmax = 0, kmin = 0, row, seg, node_this, node_prev;
	size_t n_alloc, n_alloc_b;
	double dx, dy, dz, r, r_this, r_min, x0_prev, y0_prev, x0_this, y0_this;
	double xx, yy, zz, half_dx, half_dy, zmin = DBL_MAX, zmax = -DBL_MAX;
	double *xline = NULL, *yline = NULL, *zline = NULL;
	double *x = NULL, *y = NULL, *z = NULL, *xb = NULL, *yb = NULL, *zb = NULL;
	char fname1[GMT_LEN256] = {""}, fname2[GMT_LEN256] = {""};
	FILE *fp1 = NULL, *fp2 = NULL;

	if (file) {
		sprintf (fname1, "%s.int",   file);
		sprintf (fname2, "%s.final", file);
		if ((fp1 = fopen (fname1, "w")) == NULL) {
			GMT_Report (GMT->parent, GMT_MSG_ERROR, "Unable to create file %s\n", fname1);
			return;
		}
		if ((fp2 = fopen (fname2, "w")) == NULL) {
			GMT_Report (GMT->parent, GMT_MSG_ERROR, "Unable to create file %s\n", fname1);
			fclose (fp1);
			return;
		}
	}
	/* Add constraints from breaklines */
	/* Reduce breaklines to the nearest point per node of cells crossed */

	n_alloc = n_alloc_b = GMT_INITIAL_MEM_ROW_ALLOC;
	xb = gmt_M_memory (GMT, NULL, n_alloc_b, double);
	yb = gmt_M_memory (GMT, NULL, n_alloc_b, double);
	zb = gmt_M_memory (GMT, NULL, n_alloc_b, double);

	x = gmt_M_memory (GMT, NULL, n_alloc, double);
	y = gmt_M_memory (GMT, NULL, n_alloc, double);
	z = gmt_M_memory (GMT, NULL, n_alloc, double);

	half_dx = 0.5 * C->inc[GMT_X];	half_dy = 0.5 * C->inc[GMT_Y];
	for (seg = 0; seg < T->n_segments; seg++) {
		xline = T->segment[seg]->data[GMT_X];
		yline = T->segment[seg]->data[GMT_Y];
		if (!fix_z) zline = T->segment[seg]->data[GMT_Z];
		/* 1. Interpolate the breakline to ensure there are points in every bin that it crosses */
		if (file) fprintf (fp1, "> Segment %d\n", (int)seg);
		for (row = k = 0, new_n = 1; row < T->segment[seg]->n_rows - 1; row++) {
			dx = xline[row+1] - xline[row];
			dy = yline[row+1] - yline[row];
			if (!fix_z) dz = zline[row+1] - zline[row];
			/* Given point spacing and grid spacing, how many points to interpolate? */
			n_int = lrint (hypot (dx, dy) * MAX (C->r_inc[GMT_X], C->r_inc[GMT_Y])) + 1;
			new_n += n_int;
			if (n_alloc <= new_n) {
				n_alloc += MAX (GMT_CHUNK, n_int);
				x = gmt_M_memory (GMT, x, n_alloc, double);
				y = gmt_M_memory (GMT, y, n_alloc, double);
				z = gmt_M_memory (GMT, z, n_alloc, double);
			}

			dx /= n_int;
			dy /= n_int;
			if (!fix_z) dz /= n_int;
			for (n = 0; n < n_int; k++, n++) {
				x[k] = xline[row] + n * dx;
				y[k] = yline[row] + n * dy;
				z[k] = (fix_z) ? z_level : zline[row] + n * dz;
				if (file) fprintf (fp1, "%g\t%g\t%g\n", x[k], y[k], z[k]);
			}
		}
		x[k] = xline[row];	y[k] = yline[row];	z[k] = (fix_z) ? z_level : zline[row];
		if (file) fprintf (fp1, "%g\t%g\t%g\n", x[k], y[k], z[k]);

		/* 2. Go along the (x,y,z), k = 1:new_n line and find the closest point to each bin node */
		if (file) fprintf (fp2, "> Segment %d\n", (int)seg);
		scol = (int)x_to_col (x[0], C->Grid->header->wesn[XLO], C->r_inc[GMT_X]);
		srow = (int)y_to_row (y[0], C->Grid->header->wesn[YLO], C->r_inc[GMT_Y], C->current_ny);
		node_this = row_col_to_node (srow, scol, C->current_mx);				/* The bin we are in */
		x0_this = col_to_x (scol, C->Grid->header->wesn[XLO], C->Grid->header->wesn[XHI], C->inc[GMT_X], C->current_nx);	/* Node center point */
		y0_this = row_to_y (srow, C->Grid->header->wesn[YLO], C->Grid->header->wesn[YHI], C->inc[GMT_Y], C->current_ny);
		r_min = hypot (x[0] - x0_this, y[0] - y0_this);	/* Distance from node center to start of breakline */
		xb[nb] = x[0];	yb[nb] = y[0];	zb[nb] = z[0];	/* Add this as our "nearest" breakline point (so far) for this bin */
		for (k = 1; k < new_n; k++) {
			/* Reset what is the previous point */
			node_prev = node_this;
			x0_prev = x0_this;	y0_prev = y0_this;
			scol = (int)x_to_col (x[k], C->Grid->header->wesn[XLO], C->r_inc[GMT_X]);
			srow = (int)y_to_row (y[k], C->Grid->header->wesn[YLO], C->r_inc[GMT_Y], C->current_ny);
			x0_this = col_to_x (scol, C->Grid->header->wesn[XLO], C->Grid->header->wesn[XHI], C->inc[GMT_X], C->current_nx);	/* Node center point */
			y0_this = row_to_y (srow, C->Grid->header->wesn[YLO], C->Grid->header->wesn[YHI], C->inc[GMT_Y], C->current_ny);
			node_this = row_col_to_node (srow, scol, C->current_mx);
			r_this = hypot (x[k] - x0_this, y[k] - y0_this);
			if (node_this == node_prev) {	/* Both points in same bin, see if 2nd point is closer */
				if (r_this < r_min) {	/* This point is closer than previous point */
					xb[nb] = x[k];	yb[nb] = y[k];	zb[nb] = z[k];
					r_min = r_this;
				}
			}
			/* Find point on line closest to prev bin center */
			r = surface_find_closest_point (x, y, z, k, x0_prev, y0_prev, half_dx, half_dy, &xx, &yy, &zz);
			if (r < r_min) {	/* Yes, closer than previous point */
				xb[nb] = xx;	yb[nb] = yy;	zb[nb] = zz;
				r_min = r;
			}
			if (node_this != node_prev) {	/* Find point on line closest to this bin center */
				if (file) fprintf (fp2, "%g\t%g\t%g\n", xb[nb], yb[nb], zb[nb]);
				nb++;	/* OK, moving on from this bin */
				if (nb > n_alloc_b) {
					n_alloc_b += GMT_CHUNK;
					xb = gmt_M_memory (GMT, xb, n_alloc_b, double);
					yb = gmt_M_memory (GMT, yb, n_alloc_b, double);
					zb = gmt_M_memory (GMT, zb, n_alloc_b, double);
				}
				xb[nb] = x[k];	yb[nb] = y[k];	zb[nb] = z[k];
				r_min = r_this;
				r = surface_find_closest_point (x, y, z, k, x0_this, y0_this, half_dx, half_dy, &xx, &yy, &zz);
				if (r < r_min) {	/* Yes, closer than previous point */
					xb[nb] = xx;	yb[nb] = yy;	zb[nb] = zz;
					r_min = r;
				}
			}
		}
		if (file) fprintf (fp2, "%g\t%g\t%g\n", xb[nb], yb[nb], zb[nb]);
		nb++;
	}
	if (file) {
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Reinterpolated breakline saved to file %s\n", fname1);
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Final breakline constraints saved to file %s\n", fname2);
		fclose (fp1);
		fclose (fp2);
	}

	GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Found %d breakline points, reinterpolated to %d points, reduced to %d points\n", (int)T->n_records, (int)new_n, (int)nb);

	/* Now append the interpolated breakline to the C data structure */

	k = C->npoints;
	C->data = gmt_M_memory (GMT, C->data, k+nb, struct SURFACE_DATA);
	C->z_mean *= k;		/* It was already computed, reset it to the sum so we can add more and recalculate the mean */
	if (C->set_limit[LO] == DATA)	/* Lower limit should equal minimum data found.  Start with what we have so far and change if we find lower values */
		zmin = C->limit[LO];
	if (C->set_limit[HI] == DATA)	/* Upper limit should equal maximum data found.  Start with what we have so far and change if we find higher values */
		zmax = C->limit[HI];

	for (n = 0; n < nb; n++) {

		if (gmt_M_is_dnan (zb[n])) continue;

		scol = (int)x_to_col (xb[n], C->Grid->header->wesn[XLO], C->r_inc[GMT_X]);
		if (scol < 0 || scol >= C->current_nx) continue;
		srow = (int)y_to_row (yb[n], C->Grid->header->wesn[YLO], C->r_inc[GMT_Y], C->current_ny);
		if (srow < 0 || srow >= C->current_ny) continue;

		C->data[k].index = row_col_to_index (srow, scol, C->current_nx);
		C->data[k].x = (gmt_grdfloat)xb[n];
		C->data[k].y = (gmt_grdfloat)yb[n];
		C->data[k].z = (gmt_grdfloat)zb[n];
		C->data[k].kind = SURFACE_BREAKLINE;	/* Mark as breakline constraint */
		if (zmin > zb[n]) zmin = z[n], kmin = k;
		if (zmax < zb[n]) zmax = z[n], kmax = k;
		k++;
		C->z_mean += zb[n];
	}

	if (k != (C->npoints + nb))		/* We had some NaNs */
		C->data = gmt_M_memory (GMT, C->data, k, struct SURFACE_DATA);

	C->npoints = k;
	C->z_mean /= k;

	if (C->set_limit[LO] == DATA)	/* Update our lower data-driven limit to the new minimum found */
		C->limit[LO] = C->data[kmin].z;
	if (C->set_limit[HI] == DATA)	/* Update our upper data-driven limit to the new maximum found */
		C->limit[HI] = C->data[kmax].z;

	gmt_M_free (GMT, x);
	gmt_M_free (GMT, y);
	gmt_M_free (GMT, z);
	gmt_M_free (GMT, xb);
	gmt_M_free (GMT, yb);
	gmt_M_free (GMT, zb);
}

static void *New_Ctrl (struct GMT_CTRL *GMT) {	/* Allocate and initialize a new control structure */
	struct SURFACE_CTRL *C;

	C = gmt_M_memory (GMT, NULL, 1, struct SURFACE_CTRL);

	/* Initialize values whose defaults are not 0/false/NULL */
	C->N.value = SURFACE_MAX_ITERATIONS;
	C->A.value = 1.0;	/* Real xinc == yinc in terms of distances */
	C->W.file = strdup ("surface_log.txt");
	C->Z.value = SURFACE_OVERRELAXATION;

	return (C);
}

static void Free_Ctrl (struct GMT_CTRL *GMT, struct SURFACE_CTRL *C) {	/* Deallocate control structure */
	if (!C) return;
	gmt_M_str_free (C->G.file);
	if (C->D.file) gmt_M_str_free (C->D.file);
	if (C->L.file[LO]) gmt_M_str_free (C->L.file[LO]);
	if (C->L.file[HI]) gmt_M_str_free (C->L.file[HI]);
	if (C->M.arg) gmt_M_str_free (C->M.arg);
	if (C->W.file) gmt_M_str_free (C->W.file);
	gmt_M_free (GMT, C);
}

static int usage (struct GMTAPI_CTRL *API, int level) {
	unsigned int ppm;
	const char *name = gmt_show_name_and_purpose (API, THIS_MODULE_LIB, THIS_MODULE_CLASSIC_NAME, THIS_MODULE_PURPOSE);
	if (level == GMT_MODULE_PURPOSE) return (GMT_NOERROR);
	GMT_Usage (API, 0, "usage: %s [<table>] -G%s %s %s [-A<aspect_ratio>|m] [-C<convergence_limit>] "
		"[-D<breakline>[+z[<zlevel>]]] [%s] [-Ll|u<limit>] [-M<radius>] [-N<n_iterations>] [-Q[r]] "
		"[-S<search_radius>[m|s]] [-T[b|i]<tension>] [%s] [-W[<logfile>]] [-Z<over_relaxation>] "
		"[%s] [%s] [%s] [%s] [%s] [%s] [%s] [%s] [%s] [%s] %s[%s] [%s]\n",
		name, GMT_OUTGRID, GMT_I_OPT, GMT_Rgeo_OPT, GMT_J_OPT, GMT_V_OPT, GMT_a_OPT, GMT_bi_OPT, GMT_di_OPT, GMT_e_OPT, GMT_f_OPT,
		GMT_h_OPT, GMT_i_OPT, GMT_qi_OPT, GMT_r_OPT, GMT_w_OPT, GMT_x_OPT, GMT_colon_OPT, GMT_PAR_OPT);

	if (level == GMT_SYNOPSIS) return (GMT_MODULE_SYNOPSIS);
	ppm = urint (SURFACE_CONV_LIMIT / GMT_CONV6_LIMIT);	/* Default convergence criteria */

	GMT_Message (API, GMT_TIME_NONE, "  REQUIRED ARGUMENTS:\n");
	GMT_Option (API, "<");
	gmt_outgrid_syntax (API, 'G', "Sets name of the output grid file");
	GMT_Option (API, "I,R");
	GMT_Message (API, GMT_TIME_NONE, "\n  OPTIONAL ARGUMENTS:\n");
	GMT_Usage (API, 1, "\n-A<aspect_ratio>|m");
	GMT_Usage (API, -2, "Set <aspect-ratio> [Default = 1 gives an isotropic solution], "
		"i.e., <xinc> and <yinc> are assumed to give derivatives of equal weight; if not, specify "
		"<aspect_ratio> such that <yinc> = <xinc> / <aspect_ratio>. "
		"If gridding lon,lat use -Am to set <aspect_ratio> = cosine(middle of lat range).");
	GMT_Usage (API, 1, "\n-C<convergence_limit>");
	GMT_Usage (API, -2, "Set final convergence limit; iteration stops when max |change| < <convergence_limit>. "
		"Default will choose %g of the rms of your z data after removing L2 plane (%u ppm precision). "
		"Enter your own convergence limit in the same units as your z data.", SURFACE_CONV_LIMIT, ppm);
	GMT_Usage (API, 1, "\n-D<breakline>[+z[<zlevel>]]");
	GMT_Usage (API, -2, "Use xyz data in the <breakline> file as a 'soft breakline'. Optional modifier:");
	GMT_Usage (API, 3, "+z Override any z from the <breakline> file with the appended <z_level> [0].");
	GMT_Usage (API, 1, "\n%s", GMT_J_OPT);
	GMT_Usage (API, -2, "Select the data map projection. This projection is only used to add CRS info to the "
		"grid formats that support it, i.e., netCDF, GeoTIFF, and others supported by GDAL.");
	GMT_Usage (API, 1, "\n-Ll|u<limit>");
	GMT_Usage (API, -2, "Constrain the range of output values; append directive and value, repeatable:");
	GMT_Usage (API, 3, "l: Set lower limit; forces solution to be >= <limit>.");
	GMT_Usage (API, 3, "u: Set upper limit; forces solution to be <= <limit>.");
	GMT_Usage (API, -2, "Note: <limit> can be any number, or the letter d for min (or max) input data value, "
		"or the filename of a grid with bounding values [Default solution is unconstrained]. "
		"Example: -Ll0 enforces a non-negative solution.");
	gmt_dist_syntax (API->GMT, "M<radius>", "Set maximum radius for masking the grid away from data points [no masking].");
	GMT_Usage (API, -2, "For Cartesian grids with different x and y units you may append <xlim>/<ylim>; "
		"this fills all nodes within the rectangular area of the given half-widths. "
		"One can also achieve the rectangular selection effect by using the -M<n_cells>c "
		"form. Here <n_cells> means the number of cells around the data point. As an example, "
		"-M0c means that only the cell where the point lies is retained, -M1c keeps one cell "
		"beyond that (i.e. makes a 3x3 neighborhood), and so on.");
	GMT_Usage (API, 1, "\n-N<n_iterations>");
	GMT_Usage (API, -2, "Set maximum number of iterations in the final cycle; default = %d.", SURFACE_MAX_ITERATIONS);
	GMT_Usage (API, 1, "\n-Q[r]");
	GMT_Usage (API, -2, "Query for grid sizes that might run faster than your selected -R -I, then exit. "
		"Append r to instead use the specified -R exactly as given in the calculations.");
	GMT_Usage (API, 1, "\n-S<search_radius>[m|s]");
	GMT_Usage (API, -2, "Set <search_radius> to initialize grid; default = 0 will skip this step. "
		"This step is slow and not needed unless grid dimensions are pathological; "
		"i.e., have few or no common factors. "
		"Append m or s to give <search_radius> in minutes or seconds.");
	GMT_Usage (API, 1, "\n-T[b|i]<tension>");
	GMT_Usage (API, -2, "Add tension to the gridding equation; use a value between 0 and 1. "
		"Default = 0 gives minimum curvature (smoothest; bicubic) solution. "
		"1 gives a harmonic spline solution (local max/min occur only at data points). "
		"Typically, 0.25 or more is good for potential field (smooth) data; "
		"0.5-0.75 or so for topography.  We encourage you to experiment. Optional directives:");
	GMT_Usage (API, 3, "b: Set tension in boundary conditions only.");
	GMT_Usage (API, 3, "i: Set tension in interior equations only.");
	GMT_Usage (API, -2, "Note: Without a directive we set tension for both to same value.");
	GMT_Option (API, "V");
	GMT_Usage (API, 1, "\n-W[<logfile>]");
	GMT_Usage (API, -2, "Write convergence information to a log file [surface_log.txt].");
	GMT_Usage (API, 1, "\n-Z<over_relaxation>");
	GMT_Usage (API, -2, "Change over-relaxation parameter [Default = %g]. Use a value "
		"between 1 and 2. Larger number accelerates convergence but can be unstable. "
		"Use 1 if you want to be sure to have (slow) stable convergence.", SURFACE_OVERRELAXATION);
	GMT_Option (API, "a,bi3,di,e,f,h,i,qi,r,w,:,.");
	if (gmt_M_showusage (API)) GMT_Usage (API, -2, "Note: Geographic data with 360-degree range use periodic boundary condition in longitude. "
		"For additional details, see Smith & Wessel, Geophysics, 55, 293-305, 1990.");

	return (GMT_MODULE_USAGE);
}

static int parse (struct GMT_CTRL *GMT, struct SURFACE_CTRL *Ctrl, struct GMT_OPTION *options) {
	/* Parse the options provided and set parameters in CTRL structure.
	 * Any GMT common options will override values set previously by other commands.
	 * It also replaces any file names specified as input or output with the data ID
	 * returned when registering these sources/destinations with the API.
	 */

	unsigned int n_errors = 0, k, end;
	char modifier, *c = NULL, *d = NULL;
	struct GMT_OPTION *opt = NULL;
	struct GMTAPI_CTRL *API = GMT->parent;

	for (opt = options; opt; opt = opt->next) {
		switch (opt->option) {

			case '<':	/* Skip input files */
				if (GMT_Get_FilePath (API, GMT_IS_DATASET, GMT_IN, GMT_FILE_REMOTE, &(opt->arg))) n_errors++;
				break;

			/* Processes program-specific parameters */

			case 'A':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->A.active);
				if (opt->arg[0] == 'm')
					Ctrl->A.mode = 1;
				else
					Ctrl->A.value = atof (opt->arg);
				break;
			case 'C':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->C.active);
				Ctrl->C.value = atof (opt->arg);
				if (strchr (opt->arg, '%')) {	/* Gave convergence in percent */
					Ctrl->C.mode = BY_PERCENT;
					Ctrl->C.value *= 0.01;
				}
				break;
			case 'D':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->D.active);
				if ((d = strstr (opt->arg, "+d"))) {
					d[0] = '\0';	/* Temporarily chop off +d part */
					Ctrl->D.debug = true;
				}
				if ((c = strstr (opt->arg, "+z"))) {
					c[0] = '\0';	/* Temporarily chop off +z part */
					if (c[2]) Ctrl->D.z = atof (&c[2]);	/* Get the constant z-value [0] */
					Ctrl->D.fix_z = true;
				}
				if (opt->arg[0]) Ctrl->D.file = strdup (opt->arg);
				if (GMT_Get_FilePath (API, GMT_IS_DATASET, GMT_IN, GMT_FILE_REMOTE, &(Ctrl->D.file))) n_errors++;
				if (c) c[0] = '+';	/* Restore original string */
				if (d) d[0] = '+';	/* Restore original string */
				break;
			case 'G':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->G.active);
				n_errors += gmt_get_required_file (GMT, opt->arg, opt->option, 0, GMT_IS_GRID, GMT_OUT, GMT_FILE_LOCAL, &(Ctrl->G.file));
				break;
			case 'I':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->I.active);
				n_errors += gmt_parse_inc_option (GMT, 'I', opt->arg);
				break;
			case 'J':			/* We have this gal here be cause it needs to be processed separately (after -R) */
				n_errors += gmt_M_repeated_module_option (API, Ctrl->J.active);
				n_errors += gmt_get_required_string (GMT, opt->arg, opt->option, 0, &(Ctrl->J.projstring));
				break;
			case 'L':	/* Set limits */
				switch (opt->arg[0]) {
					case 'l': case 'u':	/* Lower or upper limits  */
						end = (opt->arg[0] == 'l') ? LO : HI;	/* Which one it is */
						n_errors += gmt_M_repeated_module_option (API, Ctrl->L.active[end]);
						n_errors += gmt_M_check_condition (GMT, opt->arg[1] == 0, "Option -L%c: No argument given\n", opt->arg[0]);
						Ctrl->L.file[end] = strdup (&opt->arg[1]);
						if (!gmt_access (GMT, Ctrl->L.file[end], F_OK))	/* File exists */
							Ctrl->L.mode[end] = SURFACE;
						else if (Ctrl->L.file[end][0] == 'd')		/* Use data minimum */
							Ctrl->L.mode[end] = DATA;
						else {
							Ctrl->L.mode[end] = VALUE;		/* Use given value */
							Ctrl->L.limit[end] = atof (&opt->arg[1]);
						}
						break;
					default:
						n_errors++;
						break;
				}
				break;
			case 'M':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->M.active);
				n_errors += gmt_get_required_string (GMT, opt->arg, opt->option, 0, &(Ctrl->M.arg));
				break;
			case 'N':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->N.active);
				n_errors += gmt_get_required_uint (GMT, opt->arg, opt->option, 0, &Ctrl->N.value);
				break;
			case 'Q':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->Q.active);
				if (opt->arg[0] == 'r') Ctrl->Q.as_is = true;	/* Want to use -R as is */
				break;
			case 'S':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->S.active);
				Ctrl->S.radius = atof (opt->arg);
				Ctrl->S.unit = opt->arg[strlen(opt->arg)-1];
				if (Ctrl->S.unit == 'c' && gmt_M_compat_check (GMT, 4)) {
					GMT_Report (API, GMT_MSG_COMPAT, "Unit c for seconds is deprecated; use s instead.\n");
					Ctrl->S.unit = 's';
				}
				if (!strchr ("sm ", Ctrl->S.unit)) {
					GMT_Report (API, GMT_MSG_ERROR, "Option -S: Unrecognized unit %c\n", Ctrl->S.unit);
					n_errors++;
				}
				break;
			case 'T':
				k = 0;
				if (gmt_M_compat_check (GMT, 4)) {	/* GMT4 syntax allowed for upper case */
					modifier = opt->arg[strlen(opt->arg)-1];
					if (modifier == 'B') modifier = 'b';
					else if (modifier == 'I') modifier = 'i';
					if (!(modifier == 'b' || modifier == 'i'))
						modifier = opt->arg[0], k = 1;
				}
				else {
					modifier = opt->arg[0];
					k = 1;
				}
				if (modifier == 'b') {
					n_errors += gmt_M_repeated_module_option (API, Ctrl->T.active[BOUNDARY]);
					Ctrl->T.b_tension = atof (&opt->arg[k]);
				}
				else if (modifier == 'i') {
					n_errors += gmt_M_repeated_module_option (API, Ctrl->T.active[INTERIOR]);
					Ctrl->T.i_tension = atof (&opt->arg[k]);
				}
				else if (modifier == '.' || (modifier >= '0' && modifier <= '9')) {
					/* specification of a numeric string with no b or i directive will
					   set both tension values, meaning that we must test that neither
					   has already been set via some preceding -T specification */
					n_errors += gmt_M_repeated_module_option (API, Ctrl->T.active[BOUNDARY]);
					n_errors += gmt_M_repeated_module_option (API, Ctrl->T.active[INTERIOR]);
					Ctrl->T.i_tension = Ctrl->T.b_tension = atof (opt->arg);
				}
				else {
					GMT_Report (API, GMT_MSG_ERROR, "Option -T: Unrecognized modifier %c\n", modifier);
					n_errors++;
				}
				break;
			case 'W':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->W.active);
				if (opt->arg[0]) {	/* Specified named log file */
					gmt_M_str_free (Ctrl->W.file);
					Ctrl->W.file = strdup (opt->arg);
				}
				break;
			case 'Z':
				n_errors += gmt_M_repeated_module_option (API, Ctrl->Z.active);
				n_errors += gmt_get_required_double (GMT, opt->arg, opt->option, 0, &Ctrl->Z.value);
				break;

			default:	/* Report bad options */
				n_errors += gmt_default_option_error (GMT, opt);
				break;
		}
	}

	if (Ctrl->Q.as_is) Ctrl->Q.active = false;	/* Since -Qr does not mean a report, only -Q does */

	n_errors += gmt_M_check_condition (GMT, !GMT->common.R.active[RSET], "Must specify -R option\n");
	n_errors += gmt_M_check_condition (GMT, GMT->common.R.inc[GMT_X] <= 0.0 || GMT->common.R.inc[GMT_Y] <= 0.0,
	                                   "Option -I: Must specify positive increment(s)\n");
	n_errors += gmt_M_check_condition (GMT, Ctrl->N.value < 1, "Option -N: Max iterations must be nonzero\n");
	n_errors += gmt_M_check_condition (GMT, Ctrl->Z.value < 0.0 || Ctrl->Z.value > 2.0,
	                                   "Option -Z: Relaxation value must be 1 <= z <= 2\n");
	n_errors += gmt_M_check_condition (GMT, !Ctrl->G.file && !Ctrl->Q.active, "Option -G: Must specify output grid file\n");
	n_errors += gmt_M_check_condition (GMT, Ctrl->A.mode && gmt_M_is_cartesian (GMT, GMT_IN),
	                                   "Option -Am: Requires geographic input data\n");
	n_errors += gmt_check_binary_io (GMT, 3);

	return (n_errors ? GMT_PARSE_ERROR : GMT_NOERROR);
}

#define bailout(code) {gmt_M_free_options (mode); return (code);}
#define Return(code) {Free_Ctrl (GMT, Ctrl); gmt_end_module (GMT, GMT_cpy); bailout (code);}

EXTERN_MSC int GMT_surface (void *V_API, int mode, void *args) {
	int error = 0, key, one = 1, end;
	unsigned int old_verbose;
	char *limit[2] = {"lower", "upper"};
	double wesn[6];

	struct SURFACE_INFO C;
	struct SURFACE_CTRL *Ctrl = NULL;
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;
	struct GMT_OPTION *options = NULL;
	struct GMTAPI_CTRL *API = gmt_get_api_ptr (V_API);	/* Cast from void to GMTAPI_CTRL pointer */

	/*----------------------- Standard module initialization and parsing ----------------------*/

	if (API == NULL) return (GMT_NOT_A_SESSION);
	if (mode == GMT_MODULE_PURPOSE) return (usage (API, GMT_MODULE_PURPOSE));	/* Return the purpose of program */
	options = GMT_Create_Options (API, mode, args);	if (API->error) return (API->error);	/* Set or get option list */

	if ((error = gmt_report_usage (API, options, 0, usage)) != GMT_NOERROR) bailout (error);	/* Give usage if requested */

	/* Parse the command-line arguments */

	if ((GMT = gmt_init_module (API, THIS_MODULE_LIB, THIS_MODULE_CLASSIC_NAME, THIS_MODULE_KEYS, THIS_MODULE_NEEDS, module_kw, &options, &GMT_cpy)) == NULL) bailout (API->error); /* Save current state */
	if (GMT_Parse_Common (API, THIS_MODULE_OPTIONS, options)) Return (API->error);
	Ctrl = New_Ctrl (GMT);	/* Allocate and initialize a new control structure */
	if ((error = parse (GMT, Ctrl, options)) != 0) Return (error);

	/*---------------------------- This is the surface main code ----------------------------*/

	gmt_M_tic(GMT);
	old_verbose = GMT->current.setting.verbose;

	gmt_enable_threads (GMT);	/* Set number of active threads, if supported */
	/* Some initializations and defaults setting */
	gmt_M_memset (&C, 1, struct SURFACE_INFO);

	gmt_M_memcpy (C.wesn_orig, GMT->common.R.wesn, 4, double);	/* Save original region in case user specified -r */
	gmt_M_memcpy (wesn, GMT->common.R.wesn, 4, double);		/* Save specified region */
	C.periodic = (gmt_M_x_is_lon (GMT, GMT_IN) && gmt_M_360_range (wesn[XLO], wesn[XHI]));
	if (C.periodic && gmt_M_180_range (wesn[YLO], wesn[YHI])) {
		/* Trying to grid global geographic data - this is not something surface can do */
		GMT_Report (API, GMT_MSG_ERROR, "You are attempting to grid a global geographic data set, but surface cannot handle poles.\n");
		GMT_Report (API, GMT_MSG_ERROR, "It will do its best but it remains a Cartesian calculation which affects nodes near the poles.\n");
		GMT_Report (API, GMT_MSG_ERROR, "Because the grid is flagged as geographic, the (repeated) pole values will be averaged upon writing to file.\n");
		GMT_Report (API, GMT_MSG_ERROR, "This may introduce a jump at either pole which will distort the grid near the poles.\n");
		GMT_Report (API, GMT_MSG_ERROR, "Consider spherical gridding instead with greenspline or sphinterpolate.\n");
	}

	/* Determine if there is a better region that would allow more intermediate resolutions to converge better */
	if (!Ctrl->Q.as_is) {	/* Meaning we did not give -Qr to insist on the given -R */
		struct GMT_GRID *G = NULL;
		if ((G = GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, wesn, NULL,
			GMT_GRID_NODE_REG, GMT_NOTSET, NULL)) == NULL) Return (API->error);
		if (surface_suggest_sizes (GMT, Ctrl, G, C.factors, G->header->n_columns-1, G->header->n_rows-1, GMT->common.R.registration == GMT_GRID_PIXEL_REG)) {	/* Yes, got one */
			gmt_M_memcpy (wesn, Ctrl->Q.wesn, 4, double);		/* Save specified region */
			Ctrl->Q.adjusted = true;	/* So we know we must do the same to any -L grids */
			if (Ctrl->L.mode[LO] == SURFACE || Ctrl->L.mode[HI] == SURFACE) {
				/* Compute extra padding needed when reading -L files */
				struct GMT_GRID_HEADER_HIDDEN *HH = gmt_get_H_hidden (G->header);
				C.q_pad[XLO] = 2 + urint ((C.wesn_orig[XLO] - wesn[XLO]) * HH->r_inc[GMT_X]);
				C.q_pad[XHI] = 2 + urint ((wesn[XHI] - C.wesn_orig[XHI]) * HH->r_inc[GMT_X]);
				C.q_pad[YLO] = 2 + urint ((C.wesn_orig[YLO] - wesn[YLO]) * HH->r_inc[GMT_Y]);
				C.q_pad[YHI] = 2 + urint ((wesn[YHI] - C.wesn_orig[YHI]) * HH->r_inc[GMT_Y]);
			}
		}
		GMT_Destroy_Data (API, &G);	/* Delete the temporary grid */
	}

	/* Allocate the output grid with container only */
	if ((C.Grid = GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, wesn, NULL,
            GMT_GRID_NODE_REG, GMT_NOTSET, NULL)) == NULL) Return (API->error);

	surface_init_parameters (&C, Ctrl);	/* Pass parameters from parsing control to surface information structure C */

	if (GMT->common.R.registration == GMT_GRID_PIXEL_REG) {		/* Pixel registration request. Use the trick of offsetting area by x_inc(y_inc) / 2 */
		/* Note that the grid remains node-registered and only gets tagged as pixel-registered upon writing the final grid to file */
		wesn[XLO] += GMT->common.R.inc[GMT_X] / 2.0;	wesn[XHI] += GMT->common.R.inc[GMT_X] / 2.0;
		wesn[YLO] += GMT->common.R.inc[GMT_Y] / 2.0;	wesn[YHI] += GMT->common.R.inc[GMT_Y] / 2.0;
		/* n_columns,n_rows remain the same for now but nodes are in "pixel" position.  We reset to original wesn and reduce n_columns,n_rows by 1 when we write result */
		GMT_Destroy_Data (API, &C.Grid);	/* Delete the initial grid and recreate since wesn changed */
		if ((C.Grid = GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_CONTAINER_ONLY, NULL, wesn, NULL,
	                               GMT_GRID_NODE_REG, GMT_NOTSET, NULL)) == NULL) Return (API->error);
	}
	if (Ctrl->A.mode) Ctrl->A.value = cosd (0.5 * (wesn[YLO] + wesn[YHI]));	/* Set cos of middle latitude as aspect ratio */

	if (C.Grid->header->n_columns < 4 || C.Grid->header->n_rows < 4) {
		GMT_Report (API, GMT_MSG_ERROR, "Grid must have at least 4 nodes in each direction (you have %d by %d) - abort.\n", C.Grid->header->n_columns, C.Grid->header->n_rows);
		Return (GMT_RUNTIME_ERROR);
	}

	/* Determine the initial and intermediate grid dimensions */
	C.current_stride = gmt_gcd_euclid (C.n_columns-1, C.n_rows-1);

	if (Ctrl->Q.active && old_verbose < GMT_MSG_INFORMATION)	/* Temporarily escalate verbosity to INFORMATION */
		GMT->current.setting.verbose = GMT_MSG_INFORMATION;
	if (gmt_M_is_verbose (GMT, GMT_MSG_INFORMATION) || Ctrl->Q.active) {
		sprintf (C.format, "Grid domain: W: %s E: %s S: %s N: %s n_columns: %%d n_rows: %%d [", GMT->current.setting.format_float_out, GMT->current.setting.format_float_out, GMT->current.setting.format_float_out, GMT->current.setting.format_float_out);
		(GMT->common.R.registration == GMT_GRID_PIXEL_REG) ? strcat (C.format, "pixel registration]\n") : strcat (C.format, "gridline registration]\n");
		GMT_Report (API, GMT_MSG_INFORMATION, C.format, C.wesn_orig[XLO], C.wesn_orig[XHI], C.wesn_orig[YLO], C.wesn_orig[YHI], C.n_columns-one, C.n_rows-one);
	}
	if (C.current_stride == 1) GMT_Report (API, GMT_MSG_WARNING, "Your grid dimensions are mutually prime.  Convergence is very unlikely.\n");
	if ((C.current_stride == 1 && gmt_M_is_verbose (GMT, GMT_MSG_INFORMATION)) || Ctrl->Q.active) surface_suggest_sizes (GMT, Ctrl, C.Grid, C.factors, C.n_columns-1, C.n_rows-1, GMT->common.R.registration == GMT_GRID_PIXEL_REG);
	if (Ctrl->Q.active) {	/* Reset verbosity and bail */
		GMT->current.setting.verbose = old_verbose;
		Return (GMT_NOERROR);
	}

	/* Set current_stride = 1, read data, setting indices.  Then throw
	   away data that can't be used in the end game, limiting the
	   size of data arrays and Briggs->b[6] structure/array.  */

	C.current_stride = 1;
	surface_set_grid_parameters (&C);
	if (surface_read_data (GMT, &C, options))
		Return (GMT_RUNTIME_ERROR);
	if (Ctrl->D.active) {	/* Append breakline dataset */
		struct GMT_DATASET *Lin = NULL;
		char *file = (Ctrl->D.debug) ? Ctrl->D.file : NULL;
		if (Ctrl->D.fix_z) {	/* Either provide a fixed z value or override whatever input file may supply with this value */
			if ((error = GMT_Set_Columns (GMT->parent, GMT_IN, 2, GMT_COL_FIX_NO_TEXT)) != GMT_NOERROR)	/* Only read 2 columns */
				Return (GMT_RUNTIME_ERROR);
		}
		if ((Lin = GMT_Read_Data (API, GMT_IS_DATASET, GMT_IS_FILE, GMT_IS_LINE, GMT_READ_NORMAL, NULL, Ctrl->D.file, NULL)) == NULL)
			Return (API->error);
		if (Lin->n_columns < 2) {
			GMT_Report (API, GMT_MSG_ERROR, "Input file %s has %d column(s) but at least 2 are needed\n", Ctrl->D.file, (int)Lin->n_columns);
			Return (GMT_DIM_TOO_SMALL);
		}
		surface_interpolate_add_breakline (GMT, &C, Lin->table[0], file, Ctrl->D.fix_z, Ctrl->D.z);	/* Pass the single table since we read a single file */
	}

	surface_throw_away_unusables (GMT, &C);		/* Eliminate data points that will not serve as constraints */
	surface_remove_planar_trend (GMT, &C);		/* Fit best-fitting plane and remove it from the data; plane will be restored at the end */
	key = surface_rescale_z_values (GMT, &C);	/* Divide residual data by their rms value */

	if (GMT_Set_Comment (API, GMT_IS_GRID, GMT_COMMENT_IS_OPTION | GMT_COMMENT_IS_COMMAND, options, C.Grid) != GMT_NOERROR) Return (API->error);
	if (key == 1) {	/* Data lie exactly on a plane; write a grid with the plane and exit */
		gmt_M_free (GMT, C.data);	/* The data set is no longer needed */
		if (GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_DATA_ONLY, NULL, NULL, NULL,
		                     0, 0, C.Grid) == NULL) Return (API->error);	/* Get a grid of zeros... */
		surface_restore_planar_trend (&C);	/* ...and restore the plane we found */
		if ((error = surface_write_grid (GMT, Ctrl, &C, Ctrl->G.file)) != 0)	/* Write this grid */
			Return (error);
		Return (GMT_NOERROR);	/* Clean up and return */
	}

	if (surface_load_constraints (GMT, &C, true)) {	/* Set lower and upper constraint grids, if requested */
		gmt_M_free (GMT, C.data);
		Return (GMT_RUNTIME_ERROR);	/* Clean up and return */
	}

	/* Set up factors and reset current_stride to its initial (and largest) value  */

	C.current_stride = gmt_gcd_euclid (C.n_columns-1, C.n_rows-1);
	C.n_factors = gmt_get_prime_factors (GMT, C.current_stride, C.factors);
	surface_set_grid_parameters (&C);
	while (C.current_nx < 4 || C.current_ny < 4) {	/* Must have at least a grid of 4x4 */
		surface_smart_divide (&C);
		surface_set_grid_parameters (&C);
	}
	surface_set_offset (&C);	/* Initialize the node-jumps across rows for this grid size */
	surface_set_index (GMT, &C);	/* Determine the nearest data constraint for this grid size */

	if (Ctrl->W.active) {	/* Want to log convergence information to file */
		if ((C.fp_log = gmt_fopen (GMT, Ctrl->W.file, "w")) == NULL) {
			GMT_Report (API, GMT_MSG_ERROR, "Unable to create log file %s.\n", Ctrl->W.file);
			Return (GMT_ERROR_ON_FOPEN);
		}
		C.logging = true;
		fprintf (C.fp_log, "#grid\tmode\tgrid_iteration\tchange\tlimit\ttotal_iteration\n");
	}

	/* Now the data are ready to go for the first iteration.  */

	if (gmt_M_is_verbose (GMT, GMT_MSG_INFORMATION)) {	/* Report on memory usage for this run */
		size_t mem_use, mem_total;
		mem_use = mem_total = C.npoints * sizeof (struct SURFACE_DATA);
		GMT_Report (API, GMT_MSG_INFORMATION, "------------------------------------------\n");
		GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Memory for data array", gmt_memory_use (mem_use, 1));
		mem_use = sizeof (struct GMT_GRID) + C.mxmy * sizeof (gmt_grdfloat);	mem_total += mem_use;
		GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Memory for final grid", gmt_memory_use (mem_use, 1));
		for (end = LO; end <= HI; end++) if (C.set_limit[end]) {	/* Will need to keep a lower|upper surface constrain grid */
			mem_total += mem_use;
			GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Memory for constraint grid", gmt_memory_use (mem_use, 1));
		}
		mem_use = C.npoints * sizeof (struct SURFACE_BRIGGS) ;	mem_total += mem_use;
		GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Memory for Briggs coefficients", gmt_memory_use (mem_use, 1));
		mem_use = C.mxmy;	mem_total += mem_use;
		GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Memory for node status", gmt_memory_use (mem_use, 1));
		GMT_Report (API, GMT_MSG_INFORMATION, "------------------------------------------\n");
		GMT_Report (API, GMT_MSG_INFORMATION, "%-31s: %9s\n", "Total memory use", gmt_memory_use (mem_total, 1));
		GMT_Report (API, GMT_MSG_INFORMATION, "==========================================\n");
	}

	/* Allocate the memory needed to perform the gridding  */

	C.Briggs   = gmt_M_memory (GMT, NULL, C.npoints, struct SURFACE_BRIGGS);
	C.status   = gmt_M_memory (GMT, NULL, C.mxmy, char);
	C.fraction = gmt_M_memory (GMT, NULL, C.current_stride, double);
	if (GMT_Create_Data (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_DATA_ONLY, NULL, NULL, NULL, 0, 0, C.Grid) == NULL)
		Return (API->error);
	if (C.radius > 0) surface_initialize_grid (GMT, &C); /* Fill in nodes with a weighted average in a search radius  */
	GMT_Report (API, GMT_MSG_INFORMATION, "Grid\tMode\tIteration\tMax Change\tConv Limit\tTotal Iterations\n");

	surface_set_coefficients (GMT, &C);	/* Initialize the coefficients needed in the finite-difference expressions */

	/* Here is the main multigrid loop, were we first grid using a coarse grid and the
	 * progressively refine the grid until we reach the final configuration. */

	C.previous_stride = C.current_stride;
	surface_find_nearest_constraint (GMT, &C);		/* Assign nearest data value to nodes and evaluate Briggs coefficients */
	surface_iterate (GMT, &C, GRID_DATA);			/* Grid the data using the data constraints */

	while (C.current_stride > 1) {	/* More intermediate grids remain, go to next */
		surface_smart_divide (&C);			/* Set the new current_stride */
		surface_set_grid_parameters (&C);		/* Update node book-keeping constants */
		surface_set_offset (&C);			/* Reset the node-jumps across rows for this grid size */
		surface_set_index (GMT, &C);			/* Recompute the index values for the nearest data points */
		fill_in_forecast (GMT, &C);		/* Expand the grid and fill it via bilinear interpolation */
		surface_iterate (GMT, &C, GRID_NODES);		/* Grid again but only to improve on the bilinear guesses */
		surface_find_nearest_constraint (GMT, &C);	/* Assign nearest data value to nodes and evaluate Briggs coefficients */
		surface_iterate (GMT, &C, GRID_DATA);		/* Grid the data but now use the data constraints */
		C.previous_stride = C.current_stride;	/* Remember previous stride before we smart-divide again */
	}

	if (gmt_M_is_verbose (GMT, GMT_MSG_WARNING)) surface_check_errors (GMT, &C);	/* Report on mean misfit and curvature */

	surface_restore_planar_trend (&C);	/* Restore the least-square plane we removed earlier */

	if (Ctrl->W.active)	/* Close the log file */
		gmt_fclose (GMT, C.fp_log);

	/* Clean up after ourselves */

	gmt_M_free (GMT, C.Briggs);
	gmt_M_free (GMT, C.status);
	gmt_M_free (GMT, C.fraction);
	for (end = LO; end <= HI; end++) if (C.set_limit[end]) {	/* Free lower|upper surface constrain grids */
		if (GMT_Destroy_Data (API, &C.Bound[end]) != GMT_NOERROR)
			GMT_Report (API, GMT_MSG_ERROR, "Failed to free grid with %s bounds\n", limit[end]);
	}

	if (Ctrl->M.active) {	/* Want to mask the grid first */
		char input[GMT_VF_LEN] = {""}, mask[GMT_VF_LEN] = {""}, cmd[GMT_LEN256] = {""};
		static char *V_level = GMT_VERBOSE_CODES;
		struct GMT_GRID *Gmask = NULL;
		struct GMT_VECTOR *V = NULL;
		double *data[2] = {NULL, NULL};
		openmp_int row, col;
		uint64_t ij, dim[3] = {2, C.npoints, GMT_DOUBLE};		/* ncols, nrows, type */

		if ((V = GMT_Create_Data (API, GMT_IS_VECTOR, GMT_IS_POINT, GMT_CONTAINER_ONLY, dim, NULL, NULL, 0, 0, NULL)) == NULL) {
			Return (API->error);
		}
		for (col = 0; col < 2; col++)
			data[col] = gmt_M_memory (GMT, NULL, C.npoints, double);
		for (ij = 0; ij < C.npoints; ij++) {
			data[GMT_X][ij] = C.data[ij].x;
			data[GMT_Y][ij] = C.data[ij].y;
		}
		gmt_M_free (GMT, C.data);
		GMT_Put_Vector (API, V, GMT_X, GMT_DOUBLE, data[GMT_X]);
		GMT_Put_Vector (API, V, GMT_Y, GMT_DOUBLE, data[GMT_Y]);
		/* Create a virtual file for reading the input data grid */
		if (GMT_Open_VirtualFile (API, GMT_IS_DATASET|GMT_VIA_VECTOR, GMT_IS_POINT, GMT_IN|GMT_IS_REFERENCE, V, input) == GMT_NOTSET) {
			Return (API->error);
		}
		/* Create a virtual file to hold the mask grid */
		if (GMT_Open_VirtualFile (API, GMT_IS_GRID, GMT_IS_SURFACE, GMT_OUT|GMT_IS_REFERENCE, NULL, mask) == GMT_NOTSET) {
			Return (API->error);
		}
		gmt_disable_bghio_opts (GMT);	/* Do not want any -b -g -h -i -o to affect the reading input file in grdmask */
		/* Hardwire -rg since internally, all grids are gridline registered (until output at least) */
		sprintf (cmd, "%s -G%s -R%g/%g/%g/%g -I%g/%g -NNaN/1/1 -S%s -V%c -rg --GMT_HISTORY=readonly",
		         input, mask, wesn[XLO], wesn[XHI], wesn[YLO], wesn[YHI], GMT->common.R.inc[GMT_X],
				 GMT->common.R.inc[GMT_Y], Ctrl->M.arg, V_level[GMT->current.setting.verbose]);
		GMT_Report (API, GMT_MSG_INFORMATION, "Masking grid nodes away from data points via grdmask\n");
		GMT_Report (GMT->parent, GMT_MSG_DEBUG, "Calling grdsample with args %s\n", cmd);
		if (GMT_Call_Module (API, "grdmask", GMT_MODULE_CMD, cmd) != GMT_NOERROR) {	/* Resample the file */
			GMT_Report (API, GMT_MSG_ERROR, "Unable to mask the intermediate grid - exiting\n");
			Return (API->error);
		}
		if (GMT_Close_VirtualFile (API, input) == GMT_NOTSET) {
			Return (API->error);
		}
		if (GMT_Destroy_Data (API, &V) != GMT_NOERROR) {	/* Done with the data set */
			Return (API->error);
		}
		gmt_M_free (GMT, data[GMT_X]);	gmt_M_free (GMT, data[GMT_Y]);
		if ((Gmask = GMT_Read_VirtualFile (API, mask)) == NULL) {	/* Load in the mask grid */
			Return (API->error);
		}
		/* Apply the mask */
		gmt_M_grd_loop (GMT, Gmask, row, col, ij) C.Grid->data[ij] *= Gmask->data[ij];
		if (GMT_Destroy_Data (API, &Gmask) != GMT_NOERROR) {	/* Done with the mask */
			Return (API->error);
		}
		gmt_reenable_bghio_opts (GMT);	/* Recover settings provided by user (if -b -g -h -i were used at all) */
	}
	else
		gmt_M_free (GMT, C.data);

	/* Only gave -J to set the proj4 flag. Do it here only because otherwise would interfere with computations(!!) */
	if (Ctrl->J.active) {
		gmt_parse_common_options (GMT, "J", 'J', Ctrl->J.projstring);	/* Has to be processed independently of -R */
		C.Grid->header->ProjRefPROJ4 = gmt_export2proj4 (GMT);	/* Convert the GMT -J<...> into a proj4 string */;
		free (Ctrl->J.projstring);
	}

	if ((error = surface_write_grid (GMT, Ctrl, &C, Ctrl->G.file)) != 0)	/* Write the output grid */
		Return (error);

	gmt_M_toc(GMT,"");		/* Print total run time, but only if -Vt was set */

	Return (GMT_NOERROR);
}
