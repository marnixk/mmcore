#include "mmb_priv.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_TWOPI
#define M_TWOPI (2.0 * M_PI)
#endif

static const double chitable[51][15] = {
	{0.995, 0.99, 0.975, 0.95, 0.9, 0.5, 0.2, 0.1, 0.05, 0.025, 0.02, 0.01, 0.005, 0.002, 0.001},
	{0.0000397, 0.000157, 0.000982, 0.00393, 0.0158, 0.455, 1.642, 2.706, 3.841, 5.024, 5.412, 6.635, 7.879, 9.550, 10.828},
	{0.0100, 0.020, 0.051, 0.103, 0.211, 1.386, 3.219, 4.605, 5.991, 7.378, 7.824, 9.210, 10.597, 12.429, 13.816},
	{0.072, 0.115, 0.216, 0.352, 0.584, 2.366, 4.642, 6.251, 7.815, 9.348, 9.837, 11.345, 12.838, 14.796, 16.266},
	{0.207, 0.297, 0.484, 0.711, 1.064, 3.357, 5.989, 7.779, 9.488, 11.143, 11.668, 13.277, 14.860, 16.924, 18.467},
	{0.412, 0.554, 0.831, 1.145, 1.610, 4.351, 7.289, 9.236, 11.070, 12.833, 13.388, 15.086, 16.750, 18.907, 20.515},
	{0.676, 0.872, 1.237, 1.635, 2.204, 5.348, 8.558, 10.645, 12.592, 14.449, 15.033, 16.812, 18.548, 20.791, 22.458},
	{0.989, 1.239, 1.690, 2.167, 2.833, 6.346, 9.803, 12.017, 14.067, 16.013, 16.622, 18.475, 20.278, 22.601, 24.322},
	{1.344, 1.646, 2.180, 2.733, 3.490, 7.344, 11.030, 13.362, 15.507, 17.535, 18.168, 20.090, 21.955, 24.352, 26.124},
	{1.735, 2.088, 2.700, 3.325, 4.168, 8.343, 12.242, 14.684, 16.919, 19.023, 19.679, 21.666, 23.589, 26.056, 27.877},
	{2.156, 2.558, 3.247, 3.940, 4.865, 9.342, 13.442, 15.987, 18.307, 20.483, 21.161, 23.209, 25.188, 27.722, 29.588},
	{2.603, 3.053, 3.816, 4.575, 5.578, 10.341, 14.631, 17.275, 19.675, 21.920, 22.618, 24.725, 26.757, 29.354, 31.264},
	{3.074, 3.571, 4.404, 5.226, 6.304, 11.340, 15.812, 18.549, 21.026, 23.337, 24.054, 26.217, 28.300, 30.957, 32.909},
	{3.565, 4.107, 5.009, 5.892, 7.042, 12.340, 16.985, 19.812, 22.362, 24.736, 25.472, 27.688, 29.819, 32.535, 34.528},
	{4.075, 4.660, 5.629, 6.571, 7.790, 13.339, 18.151, 21.064, 23.685, 26.119, 26.873, 29.141, 31.319, 34.091, 36.123},
	{4.601, 5.229, 6.262, 7.261, 8.547, 14.339, 19.311, 22.307, 24.996, 27.488, 28.259, 30.578, 32.801, 35.628, 37.697},
	{5.142, 5.812, 6.908, 7.962, 9.312, 15.338, 20.465, 23.542, 26.296, 28.845, 29.633, 32.000, 34.267, 37.146, 39.252},
	{5.697, 6.408, 7.564, 8.672, 10.085, 16.338, 21.615, 24.769, 27.587, 30.191, 30.995, 33.409, 35.718, 38.648, 40.790},
	{6.265, 7.015, 8.231, 9.390, 10.865, 17.338, 22.760, 25.989, 28.869, 31.526, 32.346, 34.805, 37.156, 40.136, 42.312},
	{6.844, 7.633, 8.907, 10.117, 11.651, 18.338, 23.900, 27.204, 30.144, 32.852, 33.687, 36.191, 38.582, 41.610, 43.820},
	{7.434, 8.260, 9.591, 10.851, 12.443, 19.337, 25.038, 28.412, 31.410, 34.170, 35.020, 37.566, 39.997, 43.072, 45.315},
	{8.034, 8.897, 10.283, 11.591, 13.240, 20.337, 26.171, 29.615, 32.671, 35.479, 36.343, 38.932, 41.401, 44.522, 46.797},
	{8.643, 9.542, 10.982, 12.338, 14.041, 21.337, 27.301, 30.813, 33.924, 36.781, 37.659, 40.289, 42.796, 45.962, 48.268},
	{9.260, 10.196, 11.689, 13.091, 14.848, 22.337, 28.429, 32.007, 35.172, 38.076, 38.968, 41.638, 44.181, 47.391, 49.728},
	{9.886, 10.856, 12.401, 13.848, 15.659, 23.337, 29.553, 33.196, 36.415, 39.364, 40.270, 42.980, 45.559, 48.812, 51.179},
	{10.520, 11.524, 13.120, 14.611, 16.473, 24.337, 30.675, 34.382, 37.652, 40.646, 41.566, 44.314, 46.928, 50.223, 52.620},
	{11.160, 12.198, 13.844, 15.379, 17.292, 25.336, 31.795, 35.563, 38.885, 41.923, 42.856, 45.642, 48.290, 51.627, 54.052},
	{11.808, 12.879, 14.573, 16.151, 18.114, 26.336, 32.912, 36.741, 40.113, 43.195, 44.140, 46.963, 49.645, 53.023, 55.476},
	{12.461, 13.565, 15.308, 16.928, 18.939, 27.336, 34.027, 37.916, 41.337, 44.461, 45.419, 48.278, 50.993, 54.411, 56.892},
	{13.121, 14.256, 16.047, 17.708, 19.768, 28.336, 35.139, 39.087, 42.557, 45.722, 46.693, 49.588, 52.336, 55.792, 58.301},
	{13.787, 14.953, 16.791, 18.493, 20.599, 29.336, 36.250, 40.256, 43.773, 46.979, 47.962, 50.892, 53.672, 57.167, 59.703},
	{14.458, 15.655, 17.539, 19.281, 21.434, 30.336, 37.359, 41.422, 44.985, 48.232, 49.226, 52.191, 55.003, 58.536, 61.098},
	{15.134, 16.362, 18.291, 20.072, 22.271, 31.336, 38.466, 42.585, 46.194, 49.480, 50.487, 53.486, 56.328, 59.899, 62.487},
	{15.815, 17.074, 19.047, 20.867, 23.110, 32.336, 39.572, 43.745, 47.400, 50.725, 51.743, 54.776, 57.648, 61.256, 63.870},
	{16.501, 17.789, 19.806, 21.664, 23.952, 33.336, 40.676, 44.903, 48.602, 51.966, 52.995, 56.061, 58.964, 62.608, 65.247},
	{17.192, 18.509, 20.569, 22.465, 24.797, 34.336, 41.778, 46.059, 49.802, 53.203, 54.244, 57.342, 60.275, 63.955, 66.619},
	{17.887, 19.233, 21.336, 23.269, 25.643, 35.336, 42.879, 47.212, 50.998, 54.437, 55.489, 58.619, 61.581, 65.296, 67.985},
	{18.586, 19.960, 22.106, 24.075, 26.492, 36.336, 43.978, 48.363, 52.192, 55.668, 56.730, 59.892, 62.883, 66.633, 69.346},
	{19.289, 20.691, 22.878, 24.884, 27.343, 37.335, 45.076, 49.513, 53.384, 56.896, 57.969, 61.162, 64.181, 67.966, 70.703},
	{19.996, 21.426, 23.654, 25.695, 28.196, 38.335, 46.173, 50.660, 54.572, 58.120, 59.204, 62.428, 65.476, 69.294, 72.055},
	{20.707, 22.164, 24.433, 26.509, 29.051, 39.335, 47.269, 51.805, 55.758, 59.342, 60.436, 63.691, 66.766, 70.618, 73.402},
	{21.421, 22.906, 25.215, 27.326, 29.907, 40.335, 48.363, 52.949, 56.942, 60.561, 61.665, 64.950, 68.053, 71.938, 74.745},
	{22.138, 23.650, 25.999, 28.144, 30.765, 41.335, 49.456, 54.090, 58.124, 61.777, 62.892, 66.206, 69.336, 73.254, 76.084},
	{22.859, 24.398, 26.785, 28.965, 31.625, 42.335, 50.548, 55.230, 59.304, 62.990, 64.116, 67.459, 70.616, 74.566, 77.419},
	{23.584, 25.148, 27.575, 29.787, 32.487, 43.335, 51.639, 56.369, 60.481, 64.201, 65.337, 68.710, 71.893, 75.874, 78.750},
	{24.311, 25.901, 28.366, 30.612, 33.350, 44.335, 52.729, 57.505, 61.656, 65.410, 66.555, 69.957, 73.166, 77.179, 80.077},
	{25.041, 26.657, 29.160, 31.439, 34.215, 45.335, 53.818, 58.641, 62.830, 66.617, 67.771, 71.201, 74.437, 78.481, 81.400},
	{25.775, 27.416, 29.956, 32.268, 35.081, 46.335, 54.906, 59.774, 64.001, 67.821, 68.985, 72.443, 75.704, 79.780, 82.720},
	{26.511, 28.177, 30.755, 33.098, 35.949, 47.335, 55.993, 60.907, 65.171, 69.023, 70.197, 73.683, 76.969, 81.075, 84.037},
	{27.249, 28.941, 31.555, 33.930, 36.818, 48.335, 57.079, 62.038, 66.339, 70.222, 71.406, 74.919, 78.231, 82.367, 85.351},
	{27.991, 29.707, 32.357, 34.764, 37.689, 49.335, 58.164, 63.167, 67.505, 71.420, 72.613, 76.154, 79.490, 83.657, 86.661}
};

static int ident_start(void)
{
	return (G.p[0] >= 'A' && G.p[0] <= 'Z') ||
	       (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_';
}

static int parse_empty_array_ref(char *name, int nsz)
{
	const char *save = G.p;

	if (!ident_start())
		return 0;
	mmb_ident(name, nsz);
	mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p != '(')
	{
		G.p = save;
		return 0;
	}
	G.p++;
	mmb_skip_sp();
	if (*G.p != ')')
	{
		G.p = save;
		return 0;
	}
	G.p++;
	mmb_skip_sp();
	return 1;
}

static mmb_var *find_array(const char *name)
{
	int i;

	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used && G.vars[i].dims > 0 &&
		    mmb_keyword_eq(G.vars[i].name, name))
			return &G.vars[i];
	return 0;
}

static mmb_var *parse_array(int want_str)
{
	char name[MMB_MAX_NAME];
	mmb_var *v;

	mmb_skip_sp();
	if (!parse_empty_array_ref(name, sizeof(name)))
		mmb_syntax();
	v = find_array(name);
	if (!v)
		mmb_error("?ARRAY");
	if (want_str)
	{
		if (v->type != T_STR)
			mmb_error("?TYPE MISMATCH");
	}
	else if (v->type == T_STR)
		mmb_error("?TYPE MISMATCH");
	return v;
}

static void expect_comma(void)
{
	mmb_skip_sp();
	mmb_expect(',');
	mmb_skip_sp();
}

static double from_rad(double x)
{
	return G.opt.angle_degrees ? x * 180.0 / M_PI : x;
}

static double to_rad(double x)
{
	return G.opt.angle_degrees ? x * M_PI / 180.0 : x;
}

static int dim_len(mmb_var *v, int d)
{
	return v->dim[d] - G.opt.base + 1;
}

static int var_off(mmb_var *v, const int *idx)
{
	int off = 0, i, stride = 1;

	for (i = v->dims - 1; i >= 0; i--)
	{
		if (idx[i] < G.opt.base || idx[i] > v->dim[i])
			mmb_error("?INDEX OUT OF BOUNDS");
		off += (idx[i] - G.opt.base) * stride;
		stride *= dim_len(v, i);
	}
	return off;
}

static double array_get(mmb_var *v, int i)
{
	if (i < 0 || i >= v->size)
		mmb_error("?INDEX OUT OF BOUNDS");
	if (v->type == T_INT)
		return (double)v->data.i[i];
	return v->data.f[i];
}

static int64_t f_to_i(double x)
{
	return (int64_t)(x >= 0.0 ? x + 0.5 : x - 0.5);
}

static void array_set(mmb_var *v, int i, double x)
{
	if (i < 0 || i >= v->size)
		mmb_error("?INDEX OUT OF BOUNDS");
	if (v->type == T_INT)
		v->data.i[i] = f_to_i(x);
	else
		v->data.f[i] = x;
}

static double mat_get(mmb_var *v, int r, int c)
{
	int idx[MMB_MAX_DIMS];
	int i;

	for (i = 0; i < MMB_MAX_DIMS; i++)
		idx[i] = G.opt.base;
	idx[0] = r;
	idx[1] = c;
	return array_get(v, var_off(v, idx));
}

static void mat_set(mmb_var *v, int r, int c, double x)
{
	int idx[MMB_MAX_DIMS];
	int i;

	for (i = 0; i < MMB_MAX_DIMS; i++)
		idx[i] = G.opt.base;
	idx[0] = r;
	idx[1] = c;
	array_set(v, var_off(v, idx), x);
}

static void *tmp_alloc(unsigned n)
{
	void *p;

	if (!G.plat || !G.plat->alloc)
		mmb_error("?OUT OF MEMORY");
	p = G.plat->alloc(n);
	if (!p)
		mmb_error("?OUT OF MEMORY");
	return p;
}

static void tmp_free(void *p)
{
	if (p && G.plat && G.plat->free)
		G.plat->free(p);
}

static void shellsort(double *a, int n)
{
	int gap, i, j;
	double tmp;

	for (gap = n / 2; gap > 0; gap /= 2)
		for (i = gap; i < n; i++)
		{
			tmp = a[i];
			for (j = i; j >= gap && a[j - gap] > tmp; j -= gap)
				a[j] = a[j - gap];
			a[j] = tmp;
		}
}

static int is_pow2(int n)
{
	return n > 0 && (n & (n - 1)) == 0;
}

static void fft_radix2(double *re, double *im, int n, int inverse)
{
	int levels = 0, i, size, tmpn;
	double sign;

	for (tmpn = n; tmpn > 1; tmpn >>= 1)
		levels++;
	for (i = 0; i < n; i++)
	{
		int j = 0, b, x = i;
		double tr, ti;

		for (b = 0; b < levels; b++)
		{
			j = (j << 1) | (x & 1);
			x >>= 1;
		}
		if (j > i)
		{
			tr = re[i];
			ti = im[i];
			re[i] = re[j];
			im[i] = im[j];
			re[j] = tr;
			im[j] = ti;
		}
	}
	sign = inverse ? 1.0 : -1.0;
	for (size = 2; size <= n; size *= 2)
	{
		int half = size / 2;
		int step = n / size;

		for (i = 0; i < n; i += size)
		{
			int j, k;

			for (j = i, k = 0; j < i + half; j++, k += step)
			{
				int l = j + half;
				double ang = sign * 2.0 * M_PI * k / n;
				double wr = cos(ang), wi = sin(ang);
				double tr = re[l] * wr - im[l] * wi;
				double ti = re[l] * wi + im[l] * wr;

				re[l] = re[j] - tr;
				im[l] = im[j] - ti;
				re[j] += tr;
				im[j] += ti;
			}
		}
	}
}

static double gauss_det(double *m, int n)
{
	int i, j, k;
	double det = 1.0;

	for (i = 0; i < n; i++)
	{
		int piv = i;
		double t, maxa = fabs(m[i * n + i]);

		for (j = i + 1; j < n; j++)
			if (fabs(m[j * n + i]) > maxa)
			{
				maxa = fabs(m[j * n + i]);
				piv = j;
			}
		if (maxa == 0.0)
			return 0.0;
		if (piv != i)
		{
			for (k = 0; k < n; k++)
			{
				t = m[i * n + k];
				m[i * n + k] = m[piv * n + k];
				m[piv * n + k] = t;
			}
			det = -det;
		}
		det *= m[i * n + i];
		for (j = i + 1; j < n; j++)
		{
			double f = m[j * n + i] / m[i * n + i];
			for (k = i; k < n; k++)
				m[j * n + k] -= f * m[i * n + k];
		}
	}
	return det;
}

static int gauss_inv(const double *a, double *out, int n)
{
	double *m;
	int i, j, k;

	m = tmp_alloc((unsigned)n * (unsigned)n * 2 * sizeof(double));
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
		{
			m[i * 2 * n + j] = a[i * n + j];
			m[i * 2 * n + n + j] = (i == j) ? 1.0 : 0.0;
		}
	for (i = 0; i < n; i++)
	{
		int piv = i;
		double maxa = fabs(m[i * 2 * n + i]);

		for (j = i + 1; j < n; j++)
			if (fabs(m[j * 2 * n + i]) > maxa)
			{
				maxa = fabs(m[j * 2 * n + i]);
				piv = j;
			}
		if (maxa == 0.0)
		{
			tmp_free(m);
			return 0;
		}
		if (piv != i)
		{
			for (k = 0; k < 2 * n; k++)
			{
				double t = m[i * 2 * n + k];
				m[i * 2 * n + k] = m[piv * 2 * n + k];
				m[piv * 2 * n + k] = t;
			}
		}
		{
			double d = m[i * 2 * n + i];
			for (k = 0; k < 2 * n; k++)
				m[i * 2 * n + k] /= d;
		}
		for (j = 0; j < n; j++)
			if (j != i)
			{
				double f = m[j * 2 * n + i];
				for (k = 0; k < 2 * n; k++)
					m[j * 2 * n + k] -= f * m[i * 2 * n + k];
			}
	}
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			out[i * n + j] = m[i * 2 * n + n + j];
	tmp_free(m);
	return 1;
}

static void q_mult(const double *q1, const double *q2, double *n)
{
	double a1 = q1[0], a2 = q2[0], b1 = q1[1], b2 = q2[1];
	double c1 = q1[2], c2 = q2[2], d1 = q1[3], d2 = q2[3];

	n[0] = a1 * a2 - b1 * b2 - c1 * c2 - d1 * d2;
	n[1] = a1 * b2 + b1 * a2 + c1 * d2 - d1 * c2;
	n[2] = a1 * c2 - b1 * d2 + c1 * a2 + d1 * b2;
	n[3] = a1 * d2 + b1 * c2 - c1 * b2 + d1 * a2;
	n[4] = q1[4] * q2[4];
}

static void q_invert(const double *q, double *n)
{
	n[0] = q[0];
	n[1] = -q[1];
	n[2] = -q[2];
	n[3] = -q[3];
	n[4] = q[4];
}

static void need_quat(mmb_var *v)
{
	if (v->size != 5)
		mmb_error("?ARRAY SIZE");
}

static void load_quat(mmb_var *v, double *q)
{
	int i;

	need_quat(v);
	for (i = 0; i < 5; i++)
		q[i] = array_get(v, i);
}

static void store_quat(mmb_var *v, const double *q)
{
	int i;

	need_quat(v);
	for (i = 0; i < 5; i++)
		array_set(v, i, q[i]);
}

static void set_scalar_int(const char *name, int64_t val)
{
	int idx[MMB_MAX_DIMS];
	mmb_var *v;

	v = mmb_find_var(name, T_INT, 1, 0, idx);
	if (!v)
		mmb_error("?VARIABLE");
	if (v->type == T_INT)
		v->data.i[0] = val;
	else if (v->type == T_NUM)
		v->data.f[0] = (double)val;
	else
		mmb_error("?TYPE MISMATCH");
}

static double parse_num_arg(void)
{
	mmb_skip_sp();
	if (*G.p == '(')
	{
		double x;

		G.p++;
		x = mmb_as_float(mmb_expr());
		mmb_expect(')');
		return x;
	}
	return mmb_as_float(mmb_expr());
}

static int chi_stats(mmb_var *v, int want_p, double *out)
{
	int rows, cols, i, j, df;
	double total = 0, chi = 0, *obs, *rowsum, *colsum, *expv;

	if (v->dims != 2)
		mmb_error("?ARRAY");
	cols = dim_len(v, 0);
	rows = dim_len(v, 1);
	df = (cols - 1) * (rows - 1);
	if (df < 1)
		mmb_error("?NEEDS 2 ROWS AND COLUMNS");
	if (df > 50)
		mmb_error("?DEGREES OF FREEDOM");
	obs = tmp_alloc((unsigned)rows * (unsigned)cols * sizeof(double));
	expv = tmp_alloc((unsigned)rows * (unsigned)cols * sizeof(double));
	rowsum = tmp_alloc((unsigned)rows * sizeof(double));
	colsum = tmp_alloc((unsigned)cols * sizeof(double));
	memset(rowsum, 0, (unsigned)rows * sizeof(double));
	memset(colsum, 0, (unsigned)cols * sizeof(double));
	for (i = 0; i < rows; i++)
		for (j = 0; j < cols; j++)
		{
			double x = mat_get(v, j + G.opt.base, i + G.opt.base);

			obs[i * cols + j] = x;
			total += x;
			rowsum[i] += x;
			colsum[j] += x;
		}
	for (i = 0; i < rows; i++)
		for (j = 0; j < cols; j++)
		{
			double e = colsum[j] * rowsum[i] / total;
			double d = obs[i * cols + j] - e;

			expv[i * cols + j] = d * d / e;
			chi += expv[i * cols + j];
		}
	tmp_free(obs);
	tmp_free(expv);
	tmp_free(rowsum);
	tmp_free(colsum);
	if (want_p)
	{
		double prob = chitable[df][7];
		double chi_prob;

		if (chi > prob)
		{
			i = 7;
			while (i < 15 && chi >= chitable[df][i])
				i++;
			chi_prob = chitable[0][i - 1];
		}
		else
		{
			i = 7;
			while (i >= 0 && chi <= chitable[df][i])
				i--;
			chi_prob = chitable[0][i + 1];
		}
		*out = chi_prob * 100.0;
	}
	else
		*out = chi;
	return 1;
}

int mmb_try_math_fn(mmb_val *out)
{
	mmb_var *a, *b;
	double x, y, z;
	int i, n;

	mmb_skip_sp();
	if (mmb_match("ATAN3"))
	{
		x = parse_num_arg();
		expect_comma();
		y = parse_num_arg();
		z = atan2(y, x);
		if (z < 0.0)
			z += M_TWOPI;
		*out = mmb_num_val(from_rad(z));
		goto done;
	}
	if (mmb_match("SINH"))
	{
		*out = mmb_num_val(sinh(parse_num_arg()));
		goto done;
	}
	if (mmb_match("COSH"))
	{
		*out = mmb_num_val(cosh(parse_num_arg()));
		goto done;
	}
	if (mmb_match("TANH"))
	{
		*out = mmb_num_val(tanh(parse_num_arg()));
		goto done;
	}
	if (mmb_match("LOG10"))
	{
		x = parse_num_arg();
		if (x == 0.0)
			mmb_error("?DIVIDE BY ZERO");
		if (x < 0.0)
			mmb_error("?NEGATIVE ARGUMENT");
		*out = mmb_num_val(log10(x));
		goto done;
	}
	if (mmb_match("MAX"))
	{
		double m;
		int imax = 0;
		char iname[MMB_MAX_NAME];

		a = parse_array(0);
		m = array_get(a, 0);
		for (i = 1; i < a->size; i++)
			if (array_get(a, i) > m)
			{
				m = array_get(a, i);
				imax = i;
			}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_skip_sp();
			mmb_ident(iname, sizeof(iname));
			mmb_type_suffix(iname);
			set_scalar_int(iname, (int64_t)(imax + G.opt.base));
		}
		*out = mmb_num_val(m);
		goto done;
	}
	if (mmb_match("MIN"))
	{
		double m;
		int imin = 0;
		char iname[MMB_MAX_NAME];

		a = parse_array(0);
		m = array_get(a, 0);
		for (i = 1; i < a->size; i++)
			if (array_get(a, i) < m)
			{
				m = array_get(a, i);
				imin = i;
			}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_skip_sp();
			mmb_ident(iname, sizeof(iname));
			mmb_type_suffix(iname);
			set_scalar_int(iname, (int64_t)(imin + G.opt.base));
		}
		*out = mmb_num_val(m);
		goto done;
	}
	if (mmb_match("MEAN"))
	{
		double s = 0;

		a = parse_array(0);
		for (i = 0; i < a->size; i++)
			s += array_get(a, i);
		*out = mmb_num_val(s / (double)a->size);
		goto done;
	}
	if (mmb_match("MEDIAN"))
	{
		double *tmp, med;
		int mid;

		a = parse_array(0);
		n = a->size;
		tmp = tmp_alloc((unsigned)n * sizeof(double));
		for (i = 0; i < n; i++)
			tmp[i] = array_get(a, i);
		shellsort(tmp, n);
		mid = (n - 1) / 2;
		if (n & 1)
			med = tmp[mid];
		else
			med = (tmp[mid] + tmp[mid + 1]) / 2.0;
		tmp_free(tmp);
		*out = mmb_num_val(med);
		goto done;
	}
	if (mmb_match("SUM"))
	{
		double s = 0;

		a = parse_array(0);
		for (i = 0; i < a->size; i++)
			s += array_get(a, i);
		*out = mmb_num_val(s);
		goto done;
	}
	if (mmb_match("SD"))
	{
		double mean = 0, var = 0;

		a = parse_array(0);
		if (a->size < 2)
			mmb_error("?ARRAY SIZE");
		for (i = 0; i < a->size; i++)
			mean += array_get(a, i);
		mean /= (double)a->size;
		for (i = 0; i < a->size; i++)
		{
			double d = array_get(a, i) - mean;
			var += d * d;
		}
		*out = mmb_num_val(sqrt(var / (double)(a->size - 1)));
		goto done;
	}
	if (mmb_match("MAGNITUDE"))
	{
		double mag = 0;

		a = parse_array(0);
		for (i = 0; i < a->size; i++)
		{
			x = array_get(a, i);
			mag += x * x;
		}
		*out = mmb_num_val(sqrt(mag));
		goto done;
	}
	if (mmb_match("DOTPRODUCT"))
	{
		double s = 0;

		a = parse_array(0);
		expect_comma();
		b = parse_array(0);
		if (a->size != b->size)
			mmb_error("?SIZE MISMATCH");
		for (i = 0; i < a->size; i++)
			s += array_get(a, i) * array_get(b, i);
		*out = mmb_num_val(s);
		goto done;
	}
	if (mmb_match("M_DETERMINANT"))
	{
		int rows, cols;
		double *m, det;

		a = parse_array(0);
		if (a->dims != 2)
			mmb_error("?ARRAY MUST BE SQUARE");
		cols = dim_len(a, 0);
		rows = dim_len(a, 1);
		if (cols != rows)
			mmb_error("?ARRAY MUST BE SQUARE");
		m = tmp_alloc((unsigned)rows * (unsigned)cols * sizeof(double));
		for (i = 0; i < rows; i++)
		{
			int j;
			for (j = 0; j < cols; j++)
				m[i * cols + j] = mat_get(a, j + G.opt.base, i + G.opt.base);
		}
		det = gauss_det(m, rows);
		tmp_free(m);
		*out = mmb_num_val(det);
		goto done;
	}
	if (mmb_match("CHI_P"))
	{
		chi_stats(parse_array(0), 1, &x);
		*out = mmb_num_val(x);
		goto done;
	}
	if (mmb_match("CHI"))
	{
		chi_stats(parse_array(0), 0, &x);
		*out = mmb_num_val(x);
		goto done;
	}
	if (mmb_match("CORREL"))
	{
		double mean1 = 0, mean2 = 0, a2 = 0, b2 = 0, axb = 0;

		a = parse_array(0);
		expect_comma();
		b = parse_array(0);
		if (a->size != b->size)
			mmb_error("?SIZE MISMATCH");
		n = a->size;
		for (i = 0; i < n; i++)
		{
			mean1 += array_get(a, i);
			mean2 += array_get(b, i);
		}
		mean1 /= n;
		mean2 /= n;
		for (i = 0; i < n; i++)
		{
			double da = array_get(a, i) - mean1;
			double db = array_get(b, i) - mean2;

			a2 += da * da;
			b2 += db * db;
			axb += da * db;
		}
		*out = mmb_num_val(axb / sqrt(a2 * b2));
		goto done;
	}
	mmb_syntax();
done:
	mmb_skip_sp();
	mmb_expect(')');
	return 1;
}

static void cmd_set(void)
{
	mmb_val v;
	mmb_var *a;
	int i;

	v = mmb_expr();
	expect_comma();
	if (v.type == T_STR)
	{
		a = parse_array(1);
		for (i = 0; i < a->size; i++)
		{
			strncpy(a->data.s[i], v.s, MMB_MAX_STR);
			a->data.s[i][MMB_MAX_STR] = 0;
		}
		return;
	}
	a = parse_array(0);
	for (i = 0; i < a->size; i++)
		array_set(a, i, mmb_as_float(v));
}

static void cmd_scale_add_pow(int mode)
{
	mmb_var *src, *dst;
	double k;
	int i;

	src = parse_array(0);
	expect_comma();
	k = mmb_as_float(mmb_expr());
	expect_comma();
	dst = parse_array(0);
	if (src->size != dst->size)
		mmb_error("?SIZE MISMATCH");
	for (i = 0; i < src->size; i++)
	{
		double x = array_get(src, i);
		if (mode == 0)
			array_set(dst, i, x * k);
		else if (mode == 1)
			array_set(dst, i, x + k);
		else
			array_set(dst, i, pow(x, k));
	}
}

static void cmd_interpolate(void)
{
	mmb_var *a, *b, *c;
	double scale;
	int i;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	expect_comma();
	scale = mmb_as_float(mmb_expr());
	expect_comma();
	c = parse_array(0);
	if (a->size != b->size || a->size != c->size)
		mmb_error("?SIZE MISMATCH");
	if (a == b || a == c || b == c)
		mmb_error("?ARRAYS MUST BE DIFFERENT");
	for (i = 0; i < a->size; i++)
	{
		double t1 = array_get(a, i), t2 = array_get(b, i);
		array_set(c, i, (t2 - t1) * scale + t1);
	}
}

static void cmd_slice_insert(int insert)
{
	mmb_var *multi, *vec;
	int pos[MMB_MAX_DIMS], present[MMB_MAX_DIMS];
	int i, d, target = -1, nslice;
	int idx[MMB_MAX_DIMS];

	multi = parse_array(0);
	if (multi->dims < 2)
		mmb_error("?ARRAY");
	for (d = 0; d < multi->dims; d++)
	{
		expect_comma();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			present[d] = 0;
			pos[d] = G.opt.base;
			if (target >= 0)
				mmb_error("?ONE INDEX OMITTED");
			target = d;
		}
		else
		{
			present[d] = 1;
			pos[d] = (int)mmb_as_int(mmb_expr());
		}
	}
	if (target < 0)
		mmb_error("?ONE INDEX OMITTED");
	expect_comma();
	vec = parse_array(0);
	nslice = dim_len(multi, target);
	if (vec->size != nslice)
		mmb_error("?SIZE MISMATCH");
	for (i = 0; i < nslice; i++)
	{
		for (d = 0; d < multi->dims; d++)
			idx[d] = present[d] ? pos[d] : (i + G.opt.base);
		idx[target] = i + G.opt.base;
		if (insert)
			array_set(multi, var_off(multi, idx), array_get(vec, i));
		else
			array_set(vec, i, array_get(multi, var_off(multi, idx)));
	}
}

static void cmd_m_print(void)
{
	mmb_var *a;
	int r, c, rows, cols;

	a = parse_array(0);
	if (a->dims != 2)
		mmb_error("?ARRAY");
	cols = dim_len(a, 0);
	rows = dim_len(a, 1);
	for (r = 0; r < rows; r++)
	{
		for (c = 0; c < cols; c++)
		{
			if (c)
				mmb_out(",");
			mmb_print_val(mmb_num_val(mat_get(a, c + G.opt.base, r + G.opt.base)));
		}
		mmb_out("\n");
	}
}

static void cmd_m_transpose(void)
{
	mmb_var *a, *b;
	int r, c, rows, cols;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	if (a->dims != 2 || b->dims != 2)
		mmb_error("?ARRAY");
	cols = dim_len(a, 0);
	rows = dim_len(a, 1);
	if (dim_len(b, 0) != rows || dim_len(b, 1) != cols)
		mmb_error("?SIZE MISMATCH");
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			mat_set(b, r + G.opt.base, c + G.opt.base,
				mat_get(a, c + G.opt.base, r + G.opt.base));
}

static void cmd_m_mult(void)
{
	mmb_var *a, *b, *c;
	int i, j, k, r1, c1, r2, c2;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	expect_comma();
	c = parse_array(0);
	if (a->dims != 2 || b->dims != 2 || c->dims != 2)
		mmb_error("?ARRAY");
	c1 = dim_len(a, 0);
	r1 = dim_len(a, 1);
	c2 = dim_len(b, 0);
	r2 = dim_len(b, 1);
	if (r2 != c1)
		mmb_error("?INPUT ARRAY SIZE MISMATCH");
	if (dim_len(c, 0) != c2 || dim_len(c, 1) != r1)
		mmb_error("?OUTPUT ARRAY SIZE MISMATCH");
	if (c == a || c == b)
		mmb_error("?DESTINATION ARRAY SAME AS SOURCE");
	for (i = 0; i < r1; i++)
		for (j = 0; j < c2; j++)
		{
			double s = 0;
			for (k = 0; k < c1; k++)
				s += mat_get(a, k + G.opt.base, i + G.opt.base) *
				     mat_get(b, j + G.opt.base, k + G.opt.base);
			mat_set(c, j + G.opt.base, i + G.opt.base, s);
		}
}

static void cmd_m_inverse(void)
{
	mmb_var *a, *b;
	int n, i, j;
	double *in, *out;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	if (a->dims != 2 || b->dims != 2)
		mmb_error("?ARRAY");
	n = dim_len(a, 0);
	if (n != dim_len(a, 1) || dim_len(b, 0) != n || dim_len(b, 1) != n)
		mmb_error("?ARRAY MUST BE SQUARE");
	if (a == b)
		mmb_error("?SAME ARRAY");
	in = tmp_alloc((unsigned)n * (unsigned)n * sizeof(double));
	out = tmp_alloc((unsigned)n * (unsigned)n * sizeof(double));
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			in[i * n + j] = mat_get(a, j + G.opt.base, i + G.opt.base);
	if (!gauss_inv(in, out, n))
	{
		tmp_free(in);
		tmp_free(out);
		mmb_error("?DETERMINANT OF ARRAY IS ZERO");
	}
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			mat_set(b, j + G.opt.base, i + G.opt.base, out[i * n + j]);
	tmp_free(in);
	tmp_free(out);
}

static void cmd_v_print(void)
{
	mmb_var *a;
	int i, hex = 0;

	a = parse_array(0);
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		mmb_skip_sp();
		if (!mmb_match("HEX"))
			mmb_syntax();
		hex = 1;
		if (a->type != T_INT)
			mmb_error("?TYPE MISMATCH");
	}
	for (i = 0; i < a->size; i++)
	{
		if (i)
			mmb_out(",");
		if (hex)
		{
			char buf[24];
			int n = 0;
			uint64_t v = (uint64_t)a->data.i[i];
			char tmp[20];
			int t = 0;

			do
			{
				int d = (int)(v & 15);
				tmp[t++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
				v >>= 4;
			} while (v && t < 16);
			while (t)
				buf[n++] = tmp[--t];
			buf[n] = 0;
			mmb_out(buf);
		}
		else
			mmb_print_val(mmb_num_val(array_get(a, i)));
	}
	mmb_out("\n");
}

static void cmd_v_normalise(void)
{
	mmb_var *a, *b;
	double mag = 0;
	int i;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	if (a->size != b->size)
		mmb_error("?SIZE MISMATCH");
	for (i = 0; i < a->size; i++)
	{
		double x = array_get(a, i);
		mag += x * x;
	}
	mag = sqrt(mag);
	if (mag == 0.0)
		mmb_error("?DIVIDE BY ZERO");
	for (i = 0; i < a->size; i++)
		array_set(b, i, array_get(a, i) / mag);
}

static void cmd_v_cross(void)
{
	mmb_var *a, *b, *c;
	double u[3], v[3];
	int i;

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	expect_comma();
	c = parse_array(0);
	if (a->size != 3 || b->size != 3 || c->size != 3)
		mmb_error("?ARRAY SIZE");
	for (i = 0; i < 3; i++)
	{
		u[i] = array_get(a, i);
		v[i] = array_get(b, i);
	}
	array_set(c, 0, u[1] * v[2] - u[2] * v[1]);
	array_set(c, 1, u[2] * v[0] - u[0] * v[2]);
	array_set(c, 2, u[0] * v[1] - u[1] * v[0]);
}

static void cmd_v_mult(void)
{
	mmb_var *m, *v, *o;
	int rows, cols, i, j;

	m = parse_array(0);
	expect_comma();
	v = parse_array(0);
	expect_comma();
	o = parse_array(0);
	if (m->dims != 2)
		mmb_error("?ARRAY");
	cols = dim_len(m, 0);
	rows = dim_len(m, 1);
	if (v->size != cols || o->size != rows)
		mmb_error("?SIZE MISMATCH");
	if (o == m || o == v)
		mmb_error("?DESTINATION ARRAY SAME AS SOURCE");
	for (i = 0; i < rows; i++)
	{
		double s = 0;
		for (j = 0; j < cols; j++)
			s += mat_get(m, j + G.opt.base, i + G.opt.base) * array_get(v, j);
		array_set(o, i, s);
	}
}

static void cmd_q_invert(void)
{
	mmb_var *a, *b;
	double q[5], n[5];

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	load_quat(a, q);
	q_invert(q, n);
	store_quat(b, n);
}

static void cmd_q_vector(void)
{
	mmb_var *q;
	double x, y, z, mag, out[5];

	x = mmb_as_float(mmb_expr());
	expect_comma();
	y = mmb_as_float(mmb_expr());
	expect_comma();
	z = mmb_as_float(mmb_expr());
	expect_comma();
	q = parse_array(0);
	mag = sqrt(x * x + y * y + z * z);
	if (mag == 0.0)
		mmb_error("?DIVIDE BY ZERO");
	out[0] = 0.0;
	out[1] = x / mag;
	out[2] = y / mag;
	out[3] = z / mag;
	out[4] = mag;
	store_quat(q, out);
}

static void cmd_q_euler(void)
{
	mmb_var *q;
	double yaw, pitch, roll, s1, c1, s2, c2, s3, c3, out[5];

	yaw = -to_rad(mmb_as_float(mmb_expr()));
	expect_comma();
	pitch = to_rad(mmb_as_float(mmb_expr()));
	expect_comma();
	roll = to_rad(mmb_as_float(mmb_expr()));
	expect_comma();
	q = parse_array(0);
	s1 = sin(pitch / 2);
	c1 = cos(pitch / 2);
	s2 = sin(yaw / 2);
	c2 = cos(yaw / 2);
	s3 = sin(roll / 2);
	c3 = cos(roll / 2);
	out[1] = s1 * c2 * c3 - c1 * s2 * s3;
	out[2] = c1 * s2 * c3 + s1 * c2 * s3;
	out[3] = c1 * c2 * s3 - s1 * s2 * c3;
	out[0] = c1 * c2 * c3 + s1 * s2 * s3;
	out[4] = 1.0;
	store_quat(q, out);
}

static void cmd_q_create(void)
{
	mmb_var *q;
	double theta, x, y, z, mag, out[5], ht, s;

	theta = mmb_as_float(mmb_expr());
	expect_comma();
	x = mmb_as_float(mmb_expr());
	expect_comma();
	y = mmb_as_float(mmb_expr());
	expect_comma();
	z = mmb_as_float(mmb_expr());
	expect_comma();
	q = parse_array(0);
	ht = to_rad(theta) / 2.0;
	s = sin(ht);
	out[0] = cos(ht);
	out[1] = x * s;
	out[2] = y * s;
	out[3] = z * s;
	mag = sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
	out[0] /= mag;
	out[1] /= mag;
	out[2] /= mag;
	out[3] /= mag;
	out[4] = 1.0;
	store_quat(q, out);
}

static void cmd_q_mult(void)
{
	mmb_var *a, *b, *c;
	double q1[5], q2[5], n[5];

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	expect_comma();
	c = parse_array(0);
	load_quat(a, q1);
	load_quat(b, q2);
	q_mult(q1, q2, n);
	store_quat(c, n);
}

static void cmd_q_rotate(void)
{
	mmb_var *a, *b, *c;
	double q1[5], v1[5], temp[5], qtemp[5], n[5];

	a = parse_array(0);
	expect_comma();
	b = parse_array(0);
	expect_comma();
	c = parse_array(0);
	load_quat(a, q1);
	load_quat(b, v1);
	q_mult(q1, v1, temp);
	q_invert(q1, qtemp);
	q_mult(temp, qtemp, n);
	store_quat(c, n);
}

static void fft_load_cplx(mmb_var *src, double *re, double *im, int n)
{
	int i;

	if (src->dims == 1)
	{
		if (src->size == n)
		{
			for (i = 0; i < n; i++)
			{
				re[i] = array_get(src, i);
				im[i] = 0;
			}
			return;
		}
		if (src->size == n * 2)
		{
			for (i = 0; i < n; i++)
			{
				re[i] = array_get(src, i * 2);
				im[i] = array_get(src, i * 2 + 1);
			}
			return;
		}
	}
	if (src->dims == 2 && dim_len(src, 0) == 2 && dim_len(src, 1) == n)
	{
		for (i = 0; i < n; i++)
		{
			re[i] = mat_get(src, G.opt.base, i + G.opt.base);
			im[i] = mat_get(src, G.opt.base + 1, i + G.opt.base);
		}
		return;
	}
	mmb_error("?ARRAY SIZE");
}

static void fft_store_cplx(mmb_var *dst, const double *re, const double *im, int n)
{
	int i;

	if (dst->dims == 1 && dst->size == n * 2)
	{
		for (i = 0; i < n; i++)
		{
			array_set(dst, i * 2, re[i]);
			array_set(dst, i * 2 + 1, im[i]);
		}
		return;
	}
	if (dst->dims == 2 && dim_len(dst, 0) == 2 && dim_len(dst, 1) == n)
	{
		for (i = 0; i < n; i++)
		{
			mat_set(dst, G.opt.base, i + G.opt.base, re[i]);
			mat_set(dst, G.opt.base + 1, i + G.opt.base, im[i]);
		}
		return;
	}
	mmb_error("?ARRAY SIZE");
}

static int fft_len_from(mmb_var *v)
{
	if (v->dims == 1)
		return v->size;
	if (v->dims == 2 && dim_len(v, 0) == 2)
		return dim_len(v, 1);
	return v->size;
}

static void cmd_fft(void)
{
	mmb_var *src, *dst;
	double *re, *im;
	int n, i, mode = 0;

	mmb_skip_sp();
	if (mmb_match("MAGNITUDE"))
		mode = 1;
	else if (mmb_match("PHASE"))
		mode = 2;
	else if (mmb_match("INVERSE"))
		mode = 3;
	src = parse_array(0);
	expect_comma();
	dst = parse_array(0);
	if (mode == 3)
		n = fft_len_from(src);
	else
		n = src->dims == 1 ? src->size : fft_len_from(src);
	if (!is_pow2(n))
		mmb_error("?ARRAY SIZE MUST BE A POWER OF 2");
	re = tmp_alloc((unsigned)n * sizeof(double));
	im = tmp_alloc((unsigned)n * sizeof(double));
	if (mode == 3)
	{
		fft_load_cplx(src, re, im, n);
		for (i = 0; i < n; i++)
			im[i] = -im[i];
		fft_radix2(re, im, n, 0);
		for (i = 0; i < n; i++)
		{
			re[i] = re[i] / n;
			im[i] = -im[i] / n;
		}
		if (dst->size != n)
		{
			tmp_free(re);
			tmp_free(im);
			mmb_error("?SIZE MISMATCH");
		}
		for (i = 0; i < n; i++)
			array_set(dst, i, re[i]);
	}
	else
	{
		if (src->size != n)
		{
			tmp_free(re);
			tmp_free(im);
			mmb_error("?SIZE MISMATCH");
		}
		for (i = 0; i < n; i++)
		{
			re[i] = array_get(src, i);
			im[i] = 0;
		}
		fft_radix2(re, im, n, 0);
		if (mode == 1)
		{
			if (dst->size != n)
			{
				tmp_free(re);
				tmp_free(im);
				mmb_error("?SIZE MISMATCH");
			}
			for (i = 0; i < n; i++)
				array_set(dst, i, sqrt(re[i] * re[i] + im[i] * im[i]));
		}
		else if (mode == 2)
		{
			if (dst->size != n)
			{
				tmp_free(re);
				tmp_free(im);
				mmb_error("?SIZE MISMATCH");
			}
			for (i = 0; i < n; i++)
				array_set(dst, i, atan2(im[i], re[i]));
		}
		else
			fft_store_cplx(dst, re, im, n);
	}
	tmp_free(re);
	tmp_free(im);
}

void mmb_cmd_math(void)
{
	mmb_skip_sp();
	if (mmb_match("SET"))
	{
		cmd_set();
		return;
	}
	if (mmb_match("SCALE"))
	{
		cmd_scale_add_pow(0);
		return;
	}
	if (mmb_match("SLICE"))
	{
		cmd_slice_insert(0);
		return;
	}
	if (mmb_match("ADD"))
	{
		cmd_scale_add_pow(1);
		return;
	}
	if (mmb_match("POWER"))
	{
		cmd_scale_add_pow(2);
		return;
	}
	if (mmb_match("INTERPOLATE"))
	{
		cmd_interpolate();
		return;
	}
	if (mmb_match("INSERT"))
	{
		cmd_slice_insert(1);
		return;
	}
	if (mmb_match("M_INVERSE"))
	{
		cmd_m_inverse();
		return;
	}
	if (mmb_match("M_TRANSPOSE"))
	{
		cmd_m_transpose();
		return;
	}
	if (mmb_match("M_MULT"))
	{
		cmd_m_mult();
		return;
	}
	if (mmb_match("M_PRINT"))
	{
		cmd_m_print();
		return;
	}
	if (mmb_match("V_NORMALISE"))
	{
		cmd_v_normalise();
		return;
	}
	if (mmb_match("V_CROSS"))
	{
		cmd_v_cross();
		return;
	}
	if (mmb_match("V_MULT"))
	{
		cmd_v_mult();
		return;
	}
	if (mmb_match("V_PRINT"))
	{
		cmd_v_print();
		return;
	}
	if (mmb_match("Q_INVERT"))
	{
		cmd_q_invert();
		return;
	}
	if (mmb_match("Q_VECTOR"))
	{
		cmd_q_vector();
		return;
	}
	if (mmb_match("Q_EULER"))
	{
		cmd_q_euler();
		return;
	}
	if (mmb_match("Q_CREATE"))
	{
		cmd_q_create();
		return;
	}
	if (mmb_match("Q_MULT"))
	{
		cmd_q_mult();
		return;
	}
	if (mmb_match("Q_ROTATE"))
	{
		cmd_q_rotate();
		return;
	}
	if (mmb_match("FFT"))
	{
		cmd_fft();
		return;
	}
	mmb_syntax();
}
