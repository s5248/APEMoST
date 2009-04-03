#ifndef PARALLEL_TEMPERING_H_
#define PARALLEL_TEMPERING_H_

#include "mcmc.h"

#define DUMP_PROB_LENGTH  1000*3
#define PRINT_PROB_INTERVAL 1000


typedef struct {
	/**
	 * likelihood of acceptance (TODO: yes?)
	 */
	double beta;

	/**
	 * times this was swapped
	 */
	unsigned long swapcount;

} parallel_tempering_mcmc;

void set_beta(mcmc * m, double newbeta);

double get_beta(mcmc * m);

void inc_swapcount(mcmc * m);

unsigned long get_swapcount(mcmc * m);


void parallel_tempering(const char * filename, int n_beta,
		double beta_0, unsigned long burn_in_iterations, double rat_limit,
		unsigned long iter_limit, double mul);


#endif /* PARALLEL_TEMPERING_H_ */
