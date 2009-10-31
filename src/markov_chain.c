#include <string.h>
#include <stdio.h>
#include <libgen.h>

#include "mcmc.h"
#include "mcmc_internal.h"
#include "debug.h"
#include "gsl_helper.h"
#include <gsl/gsl_sf.h>

static void restart_from_best(mcmc * m) {
	set_params(m, dup_vector(get_params_best(m)));
	set_prob(m, -1E7);
}

void burn_in(mcmc * m, const unsigned int burn_in_iterations) {
	unsigned long iter;
	unsigned long subiter;

	gsl_vector * original_steps = get_steps(m);
	m->params_step = dup_vector(m->params_max);
	gsl_vector_sub(m->params_step, m->params_min);
	gsl_vector_scale(m->params_step, 0.1);

	debug("Beginning calibration of MCMC ...");
	debug("Starting burn-in ...");
	mcmc_check(m);
	for (iter = 0; iter < burn_in_iterations / 2; iter++) {
		for (subiter = 0; subiter < 200; subiter++) {
			markov_chain_step(m);
		}
		iter += subiter;
		dump_ul("\tBurn-in Iteration", iter);
		IFVERBOSE {
			dump_v("stepwidth", get_steps(m));
			dump_v("params", get_params(m));
		}
		mcmc_check_best(m);
	}
	debug("Re-initializing burn-in ...");
	restart_from_best(m);
	gsl_vector_scale(m->params_step, 0.5);
	for (; iter < burn_in_iterations; iter++) {
		for (subiter = 0; subiter < 200; subiter++) {
			markov_chain_step(m);
		}
		iter += subiter;
		dump_ul("\tBurn-in Iteration", iter);
		IFVERBOSE {
			dump_v("stepwidth", get_steps(m));
			dump_v("params", get_params(m));
		}
		mcmc_check_best(m);
	}
	debug("Burn-in done, adjusting steps ...");
	gsl_vector_memcpy(get_steps(m), original_steps);
	gsl_vector_free(original_steps);
	mcmc_check(m);
	debug("Burn-in done.");

}

void clear_bit(char * bitfield, unsigned int i) {
	bitfield[i / 8] &= ~(1 << (i % 8));
}
void set_bit(char * bitfield, unsigned int i) {
	bitfield[i / 8] |= (1 << (i % 8));
}
int get_bit(char * bitfield, unsigned int i) {
	return bitfield[i / 8] & (1 << (i % 8));
}

#ifndef ACCURACY_DEVIATION_FACTOR
/**
 * How good should the acceptance rate be calculated in dependence of
 * deviation from the desired value?
 * accuracy = factor * deviation
 */
#define ACCURACY_DEVIATION_FACTOR 0.25
#endif
/**
 * Get acceptance rate.
 * The closer the acceptance rate is to the desired acceptance rate, the more
 * accurately will it be assessed.
 * @return iterations used
 */
unsigned int assess_acceptance_rate(mcmc * m, unsigned int param,
		double desired_acceptance_rate, double min_accuracy,
		double * acceptance_rate, double * accuracy) {
	unsigned int i = 0;
	unsigned int j;
	unsigned int n = 40;
	unsigned int accepts = 0;
	double stdev = 0;
	unsigned int maxdev = 0;
	double accept_rate;
	double required_accuracy = min_accuracy;
	char * acceptslog = NULL;

	reset_accept_rejects(m);

	while (1) {
		IFVERBOSE
			printf("calculating %d steps.\n", n);
		acceptslog = (char*) realloc(acceptslog, n * sizeof(char));
		assert(acceptslog != NULL);

		for (; i < n; i++) {
			accepts = get_params_accepts_for(m, param);
			markov_chain_step_for(m, param);
			mcmc_check_best(m);
			if (accepts == get_params_accepts_for(m, param)) {
				/* had a reject -> set bit to 0 */
				clear_bit(acceptslog, i);
			} else {
				/* had a accept -> set bit to 1 */
				set_bit(acceptslog, i);
			}
		}
		accept_rate = accepts / (double) n;
		IFVERBOSE
			printf("accept rate: %f (%d/%d)\n", accept_rate, accepts, n);

		/* get max deviation */
		accepts = 0;
		stdev = 0;
		maxdev = 0 + 1;
		for (j = 0; j < n; j++) {
			if (get_bit(acceptslog, j) != 0) {
				accepts++;
			}
			stdev += pow(accepts - accept_rate * j, 2);
			if (abs(accepts - accept_rate * j) > maxdev) {
				maxdev = abs(accepts - accept_rate * j);
			}
		}
		stdev = sqrt(stdev / n) * 2;

		/*
		 * if we are way off, we don't need to be that accurate.
		 * if we are close, we want to be more accurate
		 * 30% could also be 10% to be more cautious
		 */
		required_accuracy = abs_double(accept_rate - desired_acceptance_rate)
				* ACCURACY_DEVIATION_FACTOR;
		if (required_accuracy < 0.005)
			required_accuracy = 0.005;
		if (required_accuracy < min_accuracy) {
			required_accuracy = min_accuracy;
		}

		/*
		 * we assume we have a deviation of maxdev at the end.
		 * how many values do we need to get below required_accuracy
		 */
		*acceptance_rate = accept_rate;
		*accuracy = maxdev / 1. / n;
		IFVERBOSE
			printf("accuracy wanted: %f, got: %f\n", required_accuracy,
					*accuracy);

		if (*accuracy <= required_accuracy) {
			break;
		}
		/*
		 * we need (maxdev / min_accuracy) values to achieve min_accuracy
		 */
		assert(maxdev / required_accuracy >= n);
		n = ((unsigned int) ((maxdev / 1. / required_accuracy) / 8) + 1) * 8;
	}
	return n;
}

#ifndef MAX_ACCURACY_IMPROVEMENT
#define MAX_ACCURACY_IMPROVEMENT 2.8
#endif

#ifndef SCALE_LIN_WORST
#define SCALE_LIN_WORST 5
#endif
#ifndef SCALE_MIN
#define SCALE_MIN 0.4
#endif

void markov_chain_calibrate_alt(mcmc * m,
		const unsigned int burn_in_iterations, double desired_acceptance_rate,
		const unsigned int iter_limit, double mul, const double adjust_step) {

	/*
	 void markov_chain_calibrate_fast(mcmc * m, const unsigned int burn_in_iterations,
	 double desired_acceptance_rate) {
	 */
	unsigned int i;
	unsigned int j;
	double current_acceptance_rate;
	double accuracy;
	unsigned int n_par = get_n_par(m);
	double scale = 1.2;
	double movedirection;
	double move;
	double max_deviation;
	double worst_accuracy = 0;
	double worst_accuracy_previous = 0;
	double best_worst_accuracy = 1;
	unsigned int iter = 0;
	FILE * progress_plot_file = fopen("calibration_progress.data", "w");
	gsl_vector * accuracies = gsl_vector_alloc(n_par);
	gsl_vector_set_all(accuracies, 0);

	mul = iter_limit + adjust_step; /* avoiding unused */

	if (desired_acceptance_rate < 0)
		desired_acceptance_rate = pow(0.25, 1.0 /* / get_n_par(m) */);

	burn_in(m, burn_in_iterations);

	/*
	 * we use assess_acceptance_rate for a n-dim point in the stepwidth-space
	 * we want to minimize | acceptance_rate - rat_limit|.
	 * if acceptance_rate >> rat_limit, we should increase the stepwidth
	 * if acceptance_rate << rat_limit, we should decrease the stepwidth
	 */

	while (1) {
		max_deviation = 0;
		/* assess current situation */
		for (j = 0; j < 1; j++) {
			printf("calculating for up to %f accuracy\n",
					worst_accuracy_previous / MAX_ACCURACY_IMPROVEMENT);
			worst_accuracy = 0;
			for (i = 0; i < n_par; i++) {
				if (gsl_vector_get(accuracies, i) < 0.1
						* worst_accuracy_previous) {
					continue;
				}

				/*
				 * the idea is to reuse the accuracy of the worst parameter in the
				 * previous round. So in this round, we only want an accuracy
				 * improvement of 3 times that. Why? This is a n-dimensional
				 * minimization problem and the parameters are not independent.
				 * So it wouldn't help us if we determined the value of one
				 * dimension extremely accurately, but be far away in another dim.
				 */
				iter += assess_acceptance_rate(m, i, desired_acceptance_rate,
						worst_accuracy_previous / MAX_ACCURACY_IMPROVEMENT,
						&current_acceptance_rate, &accuracy);
				printf("%d: a/r: %f (+-%f); desired: %f; steps: %f\n", i,
						current_acceptance_rate, accuracy,
						desired_acceptance_rate, get_steps_for_normalized(m, i));

				fprintf(progress_plot_file, "%d\t%d\t%f\t%f\t%f\n", i + 1,
						iter, get_steps_for_normalized(m, i),
						current_acceptance_rate, accuracy);

				/* keep track of worst performer */
				/*if (worst_accuracy < accuracy) {*/
				worst_accuracy += accuracy;
				/*}*/
				gsl_vector_set(accuracies, i, accuracy);

				movedirection = current_acceptance_rate
						- desired_acceptance_rate;
				/*
				 * reduce scale if we have already settled down before.
				 * We don't want the steps jumping around wildly
				 */
				scale = best_worst_accuracy * SCALE_LIN_WORST + SCALE_MIN;
				assert(scale > 0);
				move = movedirection * scale;
				if (move < -1)
					move = -0.9;
				if (max_deviation < abs_double(movedirection)) {
					max_deviation = abs_double(movedirection);
				}
				/* 10% too high => increase steps by 10% */
				/* 10% too low  => decrease steps by 10% */

				set_steps_for(m, get_steps_for(m, i) * (1 + move), i);

				printf("%d: new steps: %f\n", i, get_steps_for_normalized(m, i));
			}
			if (iter > iter_limit * n_par) {
				fprintf(stderr, "calibration failed: iteration limit reached\n");
				exit(1);
			}
			worst_accuracy_previous = worst_accuracy / n_par;
			if (worst_accuracy_previous < best_worst_accuracy) {
				best_worst_accuracy = worst_accuracy;
			}
		}
		printf("max deviation: %f; ", max_deviation);
		dump_v("current values", get_params(m));

		if (max_deviation < 0.01 && worst_accuracy < 0.02) {
			printf("small deviation: %f; quitting\n", max_deviation);
			break;
		}
	}
	fclose(progress_plot_file);

}

void markov_chain_calibrate_orig(mcmc * m,
		const unsigned int burn_in_iterations, double rat_limit,
		const unsigned int iter_limit, double mul, const double adjust_step) {
	/* we aim a acceptance rate between 20 and 30% */
	unsigned int i;

	int reached_perfection = 0;
	gsl_vector * accept_rate = NULL;
	double delta_reject_accept_t;

	unsigned long iter = 0;
	unsigned long subiter;
	int nchecks_without_rescaling = 0;
	int rescaled;

	if (rat_limit < 0)
		rat_limit = pow(0.25, 1.0 / get_n_par(m));

	burn_in(m, burn_in_iterations);
	gsl_vector_scale(m->params_step, adjust_step);
	debug("Calibrating step widths ...");
	reset_accept_rejects(m);

	while (1) {
		for (i = 0; i < get_n_par(m); i++) {
			markov_chain_step_for(m, i);
			mcmc_check_best(m);
		}
		iter++;
		if (iter % ITER_READJUST == 0) {
			accept_rate = get_accept_rate(m);

			dump_ul("------------------------------------------------ iteration", iter);
			dump_v("params", get_params(m));
			dump_v("acceptance rate: ", accept_rate);
			dump_v("steps", get_steps(m));

			rescaled = 0;
			for (i = 0; i < get_n_par(m); i++) {
				IFDEBUG
					printf(
							"\t\tneeded acceptance rate: <%f, >%f; got %f for %i",
							rat_limit + 0.05, rat_limit - 0.05, gsl_vector_get(
									accept_rate, i), i);
				if (gsl_vector_get(accept_rate, i) > rat_limit + 0.05) {
					set_steps_for(m, get_steps_for(m, i) / mul, i);
					IFDEBUG
						printf("\t scaling up   ^");
					if (rescaled == 0)
						rescaled = -1;
					if (get_steps_for_normalized(m, i) > 1) {
						printf(
								"\nWARNING: step width of %s is quite big! %d times the param space\n",
								get_params_descr(m)[i], (int) (gsl_vector_get(
										m->params_step, i) / (gsl_vector_get(
										m->params_max, i) - gsl_vector_get(
										m->params_min, i))));
						printf(
								"\nWARNING: This can mean the parameter is independent.\n");
						printf("\n SETTING PARAMETER STEP TO PARAMETER RANGE\n");
						set_steps_for_normalized(m, 1, i);
						if (rescaled == -1)
							rescaled = 0;
					}
					if (get_steps_for_normalized(m, i) > 10000) {
						fprintf(
								stderr,
								"calibration failed: step width of %s became too large.\n",
								get_params_descr(m)[i]);
						exit(1);
					}
					if (rescaled == -1)
						rescaled = 1;
				}
				if (gsl_vector_get(accept_rate, i) < rat_limit - 0.05) {
					set_steps_for(m, get_steps_for(m, i) * mul, i);
					IFDEBUG
						printf("\t scaling down v");
					if (get_steps_for_normalized(m, i) < 10E-10) {
						printf(
								"\nWARNING: step width of %s is quite small! %e times the param space\n",
								get_params_descr(m)[i],
								get_steps_for_normalized(m, i));
					}
					rescaled = 1;
				}
				IFDEBUG
					printf("\n");
				assert(gsl_vector_min(get_steps(m)) > 0);
			}
			if (rescaled == 0)
				nchecks_without_rescaling++;
			else dump_v("steps", m->params_step);
			restart_from_best(m);
			reset_accept_rejects(m);
			for (subiter = 0; subiter < ITER_READJUST; subiter++) {
				markov_chain_step(m);
				mcmc_check_best(m);
			}
			gsl_vector_free(accept_rate);
			accept_rate = get_accept_rate(m);
			dump_v("New overall accept rate after reset", accept_rate);
			gsl_vector_free(accept_rate);
			delta_reject_accept_t = get_accept_rate_global(m)
					- TARGET_ACCEPTANCE_RATE;
			dump_d("Compared to desired rate", delta_reject_accept_t);
			if (abs_double(delta_reject_accept_t) < 0.01) {
				reached_perfection = 1;
				debug("calibration reached the desired acceptance rate");
				printf("\n %d steps without rescaling \n",
						nchecks_without_rescaling);
			} else {
				reached_perfection = 0;
				if (delta_reject_accept_t < 0) {
					rat_limit /= 0.99;
				} else {
					rat_limit *= 0.99;
				}
			}
			if (nchecks_without_rescaling >= NO_RESCALING_LIMIT
					&& reached_perfection == 1 && rescaled == 0) {
				debug("quitting calibration because we did not need to rescale for several times");
				break;
			}
			if (iter > iter_limit) {
				fprintf(stderr,
						"calibration failed: limit of %d iterations reached.",
						iter_limit);
				exit(1);
			}
		}
	}
	reset_accept_rejects(m);
	debug("calibration of markov-chain done.");
}

void markov_chain_calibrate(mcmc * m, const unsigned int burn_in_iterations,
		double desired_acceptance_rate, const unsigned int iter_limit,
		double mul, const double adjust_step) {
#ifdef CALIBRATE_ALTERNATE
	markov_chain_calibrate_alt
#else
	markov_chain_calibrate_orig
#endif
	(m, burn_in_iterations, desired_acceptance_rate, iter_limit, mul,
			adjust_step);
}

void do_step_for(mcmc * m, const unsigned int i) {
	const double step = gsl_vector_get(m->params_step, i);
	const double old_value = gsl_vector_get(m->params, i);
	double new_value = old_value + get_next_gauss_random(m, step);
	const double max = gsl_vector_get(m->params_max, i);
	const double min = gsl_vector_get(m->params_min, i);
	/* dump_d("Jumping from", old_value); */

#if CIRCULAR_PARAMS == 0
	while (new_value > max || new_value < min) {
		new_value = old_value + get_next_gauss_random(m, step);
		IFVERBOSE
			printf("Value borders reached; looking for new starting point"
				" for %d \n", i);
	}
#else
	unsigned int j = 0;
	/** circular parameters **/
	unsigned int parameters[] = {CIRCULAR_PARAMS, 0};

	if (new_value > max || new_value < min) {
		while (1) {
			if (parameters[j] == 0) {
				/* non-circular parameter */
				do {
					new_value = old_value + get_next_gauss_random(m, step);
				}while (new_value > max || new_value < min);
				break;
			}
			if (parameters[j] == i + 1) {
				/* circular parameter */
				new_value = min + mod_double(new_value - min, max - min);
				break;
			}
			j++;
		}
	}
#endif
	assert(new_value <= max);
	assert(new_value >= min);
	/* dump_d("To", new_value); */
	set_params_for(m, new_value, i);
}

static void do_step(mcmc * m) {
	unsigned int i;
	for (i = 0; i < get_n_par(m); i++) {
		do_step_for(m, i);
	}
}

/**
 * @returns 1 if accept, 0 if rejecting
 */
static int check_accept(mcmc * m, const double prob_old) {
	double prob_new = get_prob(m);
	double prob_still_accept;

	/* shortcut */
	if (prob_new == prob_old) {
		return 1;
	}
	IFVERBOSE
		dump_v("suggesting parameter", get_params(m));

	if (prob_new > prob_old) {
		IFVERBOSE
			dump_d("accepting improvement of", prob_new - prob_old);
		return 1;
	} else {
		prob_still_accept = get_next_alog_urandom(m);
		if (prob_still_accept < (prob_new - prob_old)) {
			IFVERBOSE {
				dump_d("accepting probability", prob_still_accept);
				dump_d("accepting worsening of", prob_new - prob_old);
			}
			return 1;
		} else {
			IFVERBOSE
				dump_d("rejecting worsening of", prob_new - prob_old);
			return 0;
		}
	}
}

static void revert(mcmc * m, const double prob_old) {
	set_prob(m, prob_old);
}

void markov_chain_step_for(mcmc * m, const unsigned int index) {
	double prob_old = get_prob(m);
	double old_value = gsl_vector_get(m->params, index);

	mcmc_check(m);
	do_step_for(m, index);

	calc_model_for(m, index, old_value);

	if (check_accept(m, prob_old) == 1) {
		inc_params_accepts_for(m, index);
	} else {
		revert(m, prob_old);
		set_params_for(m, old_value, index);
		inc_params_rejects_for(m, index);
	}
}

#ifndef MINIMAL_STEPWIDTH
#define MINIMAL_STEPWIDTH 0.0000001
#endif
#ifndef MAXIMAL_STEPWIDTH
#define MAXIMAL_STEPWIDTH 1000000
#endif

void rmw_adapt_stepwidth(mcmc * m, const double prob_old) {
	unsigned int i;
	double step;
	double min;
	double scale;
	double max;
	double alpha = exp(get_prob(m) - prob_old);
	if (alpha > 1)
		alpha = 1;
	for (i = 0; i < get_n_par(m); i++) {
		scale = (gsl_vector_get(m->params_max, i) - gsl_vector_get(
				m->params_min, i));
		min = MINIMAL_STEPWIDTH * scale;
		max = MAXIMAL_STEPWIDTH * scale;

		step = gsl_vector_get(get_steps(m), i);
		step += get_next_uniform_random(m) / sqrt(m->n_iter) * (alpha
				- TARGET_ACCEPTANCE_RATE) * scale;
		;
		if (step < min)
			step = min;
		if (step > max)
			step = max;
		gsl_vector_set(get_steps(m), i, step);
	}
}

void markov_chain_step(mcmc * m) {
	double prob_old = get_prob(m);
	gsl_vector * old_values = dup_vector(m->params);

	mcmc_check(m);
	do_step(m);

	calc_model(m, old_values);

	if (check_accept(m, prob_old) == 1) {
		inc_params_accepts(m);
		gsl_vector_free(old_values);
	} else {
		revert(m, prob_old);
		set_params(m, old_values);
		inc_params_rejects(m);
	}
}
