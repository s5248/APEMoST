#include <signal.h>
#include <gsl/gsl_sf.h>

#include "mcmc.h"
#include "parallel_tempering.h"
#include "debug.h"

#ifndef SIGMA
#define SIGMA 0.5
#endif

double apply_formula(mcmc * m, unsigned int i, double param0, double param1,
		double param2) {
	double x = gsl_matrix_get(m->data, i, 0);
	double y = param0 * gsl_sf_sin(2.0 * M_PI * param1 * x + param2);
	return y;
}

void calc_model(mcmc * m, const gsl_vector * old_values) {
	unsigned int i;
	double param0 = gsl_vector_get(m->params, 0);
	double param1 = gsl_vector_get(m->params, 1);
	double param2 = gsl_vector_get(m->params, 2);
	double y;
	double deltay;
	double square_sum = 0;

	(void) old_values;
	/*dump_v("recalculating model for parameter values", m->params);*/
	for (i = 0; i < m->data->size1; i++) {
		y = gsl_matrix_get(m->data, i, 1);
		deltay = apply_formula(m, i, param0, param1, param2) - y;
		square_sum += deltay * deltay;
	}
	set_prob(m, get_beta(m) * square_sum / (-2 * SIGMA * SIGMA));
	/*debug("model done");*/
}

void calc_model_for(mcmc * m, const unsigned int i, const double old_value) {
	(void) i;
	(void) old_value;

	calc_model(m, NULL);
}

