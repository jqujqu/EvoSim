/* Copyright (C) 2018 University of Southern California
 *                    Jianghan Qu, Andrew D Smith and Xiaojing Ji
 *
 * Author: Andrew D. Smith, Jianghan Qu and Xiaojing Ji
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>   /* exp, sqrt, pow fabs*/
#include <numeric>  /* std::inner_product, accumulate*/
#include <functional>

#include "OptionParser.hpp"
#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"

#include "PhyloTreePreorder.hpp"
#include "Path.hpp"
#include "StateSeq.hpp"
#include "EpiEvoModel.hpp"
#include "TreeHelper.hpp"

using std::vector;
using std::endl;
using std::cerr;
using std::cout;
using std::string;


template <class T, class U> static void
scale_mult(const T scale_factor, U &to_scale) {
  transform(to_scale.begin(), to_scale.end(), to_scale.begin(),
            std::bind(std::multiplies<double>(),
                      std::placeholders::_1,
                      scale_factor));
}


template <class T, class U> static void
scale_div(const T scale_factor, U &to_scale) {
  transform(to_scale.begin(), to_scale.end(), to_scale.begin(),
            std::bind(std::divides<double>(),
                      std::placeholders::_1,
                      scale_factor));
}


static void
add_sufficient_statistics(const vector<Path> &paths,
                          vector<double> &J, vector<double> &D) {
  // iterate over sites with valid triples (i.e. not the first and last)
  const size_t n_sites = paths.size();
  assert(n_sites > 0); // make sure the root isn't processed
  for (size_t i = 1; i < n_sites - 1; ++i)
    add_sufficient_statistics(paths[i-1], paths[i], paths[i+1], J, D);
}

static void
get_sufficient_statistics(const vector<vector<Path> > &all_paths,
                          vector<double> &J, vector<double> &D) {
  static const size_t n_triplets = 8;
  J = vector<double>(n_triplets, 0.0);
  D = vector<double>(n_triplets, 0.0);

  // iterate over nodes, starting at 1 to avoid root
  for (size_t i = 1; i < all_paths.size(); ++i)
    add_sufficient_statistics(all_paths[i], J, D);
}

/* branches in likelihood and gradient ascent steps are scalers that
   should be multiplied to input branch lengths after the optimization
   is over */
static void
get_sufficient_statistics(const vector<vector<Path> > &all_paths,
                          vector<vector<double> > &J,
                          vector<vector<double> > &D) {

  static const size_t n_triplets = 8;
  const size_t n_branches = all_paths.size();
  J = vector<vector<double> >(n_branches, vector<double>(n_triplets, 0.0));
  D = vector<vector<double> >(n_branches, vector<double>(n_triplets, 0.0));

  // iterate over nodes, starting at 1 to avoid root
  for (size_t i = 1; i < n_branches; ++i)
    add_sufficient_statistics(all_paths[i], J[i], D[i]);
}


static double
log_likelihood(const vector<double> &J, const vector<double> &D,
               const vector<double> &rates) {

  static const size_t n_triplets = 8;
  assert(J.size() == n_triplets && D.size() == n_triplets &&
         rates.size() == n_triplets);

  double ll = 0;
  for (size_t i = 0; i < n_triplets; ++i)
    ll += J[i]*log(rates[i]) - D[i]*rates[i];

  return ll;
}


static double
log_likelihood(const vector<vector<double> > &J,
               const vector<vector<double> > &D,
               const vector<double> &rates,
               const vector<double> &branches) {

  double ll = 0;
  for (size_t b = 1; b < branches.size(); ++b) {
    vector<double> scaled_rates(rates);
    scale_mult(branches[b], scaled_rates);
    ll += log_likelihood(J[b], D[b], scaled_rates);
  }
  return ll;
}


/* compute gradients wrt log(rate[i]) */
static void
add_to_gradient(const vector<double> &J, const vector<double> &D,
                const vector<double> &rates, vector<double> &gradient) {
  static const size_t n_params = 8;
  assert(gradient.size() == n_params);

  /* ordering of parameters:
     000 -> 010: 0 birth
     010 -> 000: 2 death
     001 -> 011: 1 expansion
     011 -> 001: 3 contraction
     100 -> 110: 4 expansion
     110 -> 100: 6 contraction
     101 -> 111: 5 merging
     111 -> 101: 7 splitting (not a free parameter)
  */
  const double factor_111 = (J[7] - D[7]*rates[7]);

  // [0] BIRTH: parameter 0 corresponds to 000->010
  gradient[0] += J[0] - D[0]*rates[0] + factor_111;

  // [2] DEATH: parameter 2 corresponds to 010->000
  gradient[2] += J[2] - D[2]*rates[2] - factor_111;

  // [1, 4] EXPANSION: parameter 1 corresponds to 001->011
  gradient[1] += J[1] + J[4] - (D[1] + D[4])*rates[1] - 2*factor_111;
  gradient[4] = gradient[1]; // expansion in other direction: 100->110

  // [3, 6] CONTRACTION: parameter 3 corresponds to 011->001
  gradient[3] += J[3] + J[6] - (D[3] + D[6])*rates[3] + 2*factor_111;
  gradient[6] = gradient[3]; // contraction in other direction: 110->100

  // [5] MERGING: parameter 2 corresponds to 101->111
  gradient[5] += J[5] - D[5]*rates[5] + factor_111;

  // [7] SPLITTING: parameter 7 corresponds to 111->101
  /* gradient[7] remains 0, since rates[7] is determined by other rates */
}

/* compute gradients wrt log(rate[i]) */
static void
get_gradient(const vector<double> &J, const vector<double> &D,
             const vector<double> &rates, vector<double> &gradient) {
  static const size_t n_params = 8;
  gradient = vector<double>(n_params, 0.0);
  add_to_gradient(J, D, rates, gradient);
}


/* compute gradients wrt log(rate[i]) and log(branches[b])*/
static void
get_gradient(const vector<vector<double> > &J,
             const vector<vector<double> > &D,
             const vector<double> &rates,
             const vector<double> &branches,
             vector<double> &gradient) {

  static const size_t n_params = 8;
  gradient = vector<double>(n_params, 0.0);

  vector<double> scaled_rates(rates.size(), 0.0);
  for (size_t b = 1; b < branches.size(); ++b) {
    copy(rates.begin(), rates.end(), scaled_rates.begin());
    scale_mult(branches[b], scaled_rates);
    add_to_gradient(J[b], D[b], scaled_rates, gradient);
  }
}


/* Propose a set of new candidate rate by using the given gradient
 * vector and step size.
 */
static void
candidate_rates(const double step_size,
                const vector<double> &gradient,
                const vector<double> &rates,
                vector<double> &updated_rates) {

  static const size_t n_rates = 8;

  updated_rates.resize(n_rates, 0);
  for (size_t i = 0; i < n_rates - 1; ++i)
    updated_rates[i] = exp(log(rates[i]) + gradient[i]*step_size);

  // final rate is in terms of other rates
  updated_rates[n_rates - 1] =
    exp(    log(updated_rates[0])   // 000 -> 010 (once, numerator)
        +   log(updated_rates[5])   // 101 -> 111 (once, numerator)
        + 2*log(updated_rates[3])   // 011 -> 001 (twice,numerator)
        -   log(updated_rates[2])   // 010 -> 000 (once, denominator)
        - 2*log(updated_rates[1])); // 001 -> 011 (twice,denominator)
}


/* This function calls the candidate_rates function to obtain new
   rates, and then updates the branch lengths correspondingly.
 */
static void
candidate_branches(const vector<vector<double> > &J,
                   const vector<vector<double> > &D,
                   const vector<double> &rates,
                   vector<double> &updated_branches) {

  assert(J.size() == D.size());
  const size_t n_branches = D.size();

  updated_branches.resize(n_branches);
  for (size_t b = 1; b < n_branches; ++b) {
    const double num = accumulate(J[b].begin(), J[b].end(), 0.0);
    const double denom = inner_product(D[b].begin(), D[b].end(),
                                       rates.begin(), 0.0);
    updated_branches[b] = num/denom;
  }
}


/* This function calls the candidate_rates function to obtain new
   rates, and then updates the branch lengths correspondingly.
 */
static void
candidate_rates_and_branches(const double step_size,
                             const vector<vector<double> > &J,
                             const vector<vector<double> > &D,
                             const vector<double> &gradient,
                             const vector<double> &rates,
                             const vector<double> &branches,
                             vector<double> &updated_rates,
                             vector<double> &updated_branches) {
  // ADS: does it matter which of these updates happens first?
  candidate_rates(step_size, gradient, rates, updated_rates);
  candidate_branches(J, D, updated_rates, updated_branches);
}


static double
get_starting_step_size(const vector<double> &gradient) {
  double l1_norm = 0.0;
  for (size_t i = 0; i < gradient.size(); ++i)
    l1_norm += fabs(gradient[i]);
  return 1.0/l1_norm; // MAGIC!!!!
}


/* makes a single step of gradient ascent; identifies a new set of
 * rates using the summary statistics in J and D. The return value
 * indicates if the ascent was able to find a better likelihood.
 */
static bool
gradient_ascent(const double param_tol,
                const vector<double> &J, const vector<double> &D,
                const double llk, const vector<double> &rates,
                double &updated_llk, vector<double> &updated_rates) {

  assert(llk == log_likelihood(J, D, rates));

  /* compute llk and gradient */
  vector<double> gradient;
  get_gradient(J, D, rates, gradient);

  double step_size = get_starting_step_size(gradient);
  updated_llk = std::numeric_limits<double>::lowest();
  while (updated_llk < llk && step_size > param_tol) {
    candidate_rates(step_size, gradient, rates, updated_rates);
    updated_llk = log_likelihood(J, D, updated_rates);
    step_size *= 0.5;
  }
  return (updated_llk > llk);
}


/* makes a single step of gradient ascent; identifies a new set of
   rates and branch scalers using the summary statistics in J and D */
static bool
gradient_ascent(const double param_tol,
                const vector<vector<double> > &J,
                const vector<vector<double> > &D,
                const double llk,
                const vector<double> &rates,
                const vector<double> &branches,
                double &updated_llk,
                vector<double> &updated_rates,
                vector<double> &updated_branches) {

  assert(llk == log_likelihood(J, D, rates, branches));

  /* compute llk and gradient */
  vector<double> gradient;
  get_gradient(J, D, rates, branches, gradient);

  double step_size = get_starting_step_size(gradient);
  updated_llk = std::numeric_limits<double>::lowest();
  while (updated_llk < llk && step_size > param_tol) {
    candidate_rates_and_branches(step_size, J, D, gradient, rates, branches,
                                 updated_rates, updated_branches);
    updated_llk = log_likelihood(J, D, updated_rates, updated_branches);
    step_size *= 0.5;
  }
  return (updated_llk > llk);
}


static double
estimate_rates(const double param_tol,
               const vector<double> &J, const vector<double> &D,
               const vector<double> &input_rates,
               vector<double> &rates) {

  double llk = log_likelihood(J, D, input_rates);
  rates = input_rates;

  vector<double> tmp_rates(rates);
  double tmp_llk = llk;
  while (gradient_ascent(param_tol, J, D, llk, rates, tmp_llk, tmp_rates)) {
    llk = tmp_llk;
    rates.swap(tmp_rates);
  }
  return llk;
}

/* Estimate rates and branches
 */
static double
estimate_rates_and_branches(const double param_tol,
                            const vector<vector<double> > &J,
                            const vector<vector<double> > &D,
                            const vector<double> &input_rates,
                            const vector<double> &input_branches,
                            vector<double> &rates,
                            vector<double> &branches) {

  double llk = log_likelihood(J, D, input_rates, input_branches);
  rates = input_rates;
  branches = input_branches;

  vector<double> tmp_rates(rates);
  vector<double> tmp_branches(branches);
  double tmp_llk = llk;
  while (gradient_ascent(param_tol, J, D, llk, rates, branches, tmp_llk,
                         tmp_rates, tmp_branches)) {
    llk = tmp_llk;
    rates.swap(tmp_rates);
    branches.swap(tmp_branches);
  }
  return llk;
}

static void
set_one_change_per_site_per_unit_time(vector<double> &rates,
                                      vector<double> &branches) {

  /* scale rates and branches to have unit branch length
     corresponding to one expected transition per site */
  const double the_rate_factor = rate_scaling_factor(rates);

  // scale up the branch lengths
  scale_mult(the_rate_factor, branches);

  // scale up the D values (total time spent in each state)
  // scale_mult(the_rate_factor, D);

  // scale down the rates
  scale_div(the_rate_factor, rates);
}


static void
compute_estimates_for_rates_only(const bool VERBOSE,
                                 const double param_tol,
                                 const vector<vector<Path> > &all_paths,
                                 const TreeHelper &th,
                                 EpiEvoModel &the_model) {
  if (VERBOSE)
    cerr << "[COMPUTING INITIAL SUFFICIENT STATISTICS]" << endl;
  vector<double> J;
  vector<double> D;
  get_sufficient_statistics(all_paths, J, D);

  if (VERBOSE)
    cerr << "J:" << endl << triplet_info_to_string(J) << endl
         << "D:" << endl << triplet_info_to_string(D) << endl
         << "initial log-likelihood: "
         << log_likelihood(J, D, the_model.triplet_rates) << endl;

  vector<double> updated_rates;
  estimate_rates(param_tol, J, D, the_model.triplet_rates, updated_rates);

  the_model.rebuild_from_triplet_rates(updated_rates);

  if (VERBOSE)
    cerr << "updated rates:\n"
         << triplet_info_to_string(the_model.triplet_rates) << endl
         << "updated log-likelihood: "
         << log_likelihood(J, D, the_model.triplet_rates) << endl;
}


static void
compute_estimates_rates_and_branches(const bool VERBOSE,
                                     const double param_tol,
                                     vector<vector<Path> > &all_paths,
                                     TreeHelper &th,
                                     EpiEvoModel &the_model) {
  if (VERBOSE)
    cerr << "[NORMALIZING ALL PATH LENGTHS]" << endl;
  for (size_t b = 1; b < all_paths.size(); ++b)
    for (size_t i = 0; i < all_paths[b].size(); ++i)
      all_paths[b][i].scale_to_unit_length();

  // get initial values of sufficient statistics
  if (VERBOSE)
    cerr << "[COMPUTING INITIAL SUFFICIENT STATISTICS]" << endl;
  vector<vector<double> > J;
  vector<vector<double> > D;
  get_sufficient_statistics(all_paths, J, D);
  if (VERBOSE) {
    for (size_t b = 1; b < all_paths.size(); ++b)
      cerr << "J[" << b << "]" << endl << triplet_info_to_string(J[b]) << endl
           << "D[" << b << "]" << endl << triplet_info_to_string(D[b]) << endl;
    cerr << "initial log-likelihood: "
         << log_likelihood(J, D, the_model.triplet_rates, th.branches) << endl;
  }

  vector<double> updated_rates;
  vector<double> updated_branches;
  estimate_rates_and_branches(param_tol, J, D,
                              the_model.triplet_rates, th.branches,
                              updated_rates, updated_branches);

  set_one_change_per_site_per_unit_time(updated_rates, updated_branches);
  the_model.rebuild_from_triplet_rates(updated_rates);
  th.branches = updated_branches;

  if (VERBOSE) {
    cerr << "updated rates:\n" << triplet_info_to_string(the_model.triplet_rates) << endl
         << "updated_branches:" << endl;
    for (size_t i = 0; i < th.node_names.size(); ++i)
      cerr << th.node_names[i] << '\t' << th.branches[i] << endl;
    cerr << "updated log-likelihood: "
         << log_likelihood(J, D, the_model.triplet_rates, th.branches) << endl;
  }
}


static void
estimate_root_distribution(const vector<vector<Path> > &all_paths,
                           EpiEvoModel &the_model) {
  assert(all_paths.size() >= 2 &&
         !all_paths[1].empty());

  size_t prev = all_paths[1].front().init_state;
  for (size_t i = 1; i < all_paths[1].size(); ++i) {
    const size_t curr = all_paths[1][i].init_state;
    the_model.init_T[prev][curr]++;
    prev = curr;
  }
  double tot = the_model.init_T[0][0] + the_model.init_T[0][1];
  the_model.init_T[0][0] /= tot;
  the_model.init_T[0][1] /= tot;
  tot = the_model.init_T[1][0] + the_model.init_T[1][1];
  the_model.init_T[1][0] /= tot;
  the_model.init_T[1][1] /= tot;
}


int main(int argc, const char **argv) {

  try {

    static const double param_tol = 1e-10;

    string outfile;
    bool VERBOSE = false;
    bool OPTBRANCH = false;
    bool scale_the_rates = true;
    string param_file;
    string tree_file;

    ////////////////////////////////////////////////////////////////////////
    OptionParser opt_parse(strip_path(argv[0]), "estimate parameters from"
                           " complete data (site-specific paths)",
                           "<path-file>");
    opt_parse.add_opt("param", 'p', "initial parameter file",
                      true, param_file);
    opt_parse.add_opt("tree", 't', "initial tree file in newick format",
                      true, tree_file);
    opt_parse.add_opt("verbose", 'v', "print more run info",
                      false, VERBOSE);
    opt_parse.add_opt("no-rate-scaling", 'S', "do not scale rates",
                      false, scale_the_rates);
    opt_parse.add_opt("branch", 'b', "optimize branch lengths as well",
                      false, OPTBRANCH);
    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (argc == 1 || opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (leftover_args.size() != 1) {
      cerr << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    const string path_file(leftover_args.front());
    ////////////////////////////////////////////////////////////////////////

    if (VERBOSE)
      cerr << "[READING PATHS FILE: " << path_file << "]" << endl;
    vector<vector<Path> > all_paths; // along multiple branches
    vector<string> node_names;
    read_paths(path_file, node_names, all_paths);
    const size_t n_sites = all_paths.back().size();

    if (VERBOSE)
      cerr << "[READING PARAMETER FILE: " << param_file << endl;
    EpiEvoModel the_model;
    read_model(param_file, the_model);
    if (VERBOSE)
      cerr << the_model << endl;

    if (VERBOSE)
      cerr << "[READING TREE FILE: " << tree_file << "]" << endl;
    PhyloTreePreorder the_tree; // tree topology and branch lengths
    std::ifstream tree_in(tree_file.c_str());
    if (!tree_in || !(tree_in >> the_tree))
      throw std::runtime_error("bad tree file: " + tree_file);
    const size_t n_nodes = the_tree.get_size();
    TreeHelper th(the_tree);

    if (VERBOSE) {
      cerr << "[INITIAL GUESS AT BRANCH LENGTHS]" << endl;
      for (size_t i = 0; i < th.node_names.size(); ++i)
        cerr << th.node_names[i] << '\t' << th.branches[i] << endl;
    }

    if (VERBOSE)
      cerr << "n_nodes=" << n_nodes << endl
           << "n_sites=" << n_sites << endl;

    if (!OPTBRANCH) {
      compute_estimates_for_rates_only(VERBOSE, param_tol,
                                       all_paths, th, the_model);
    }
    else
      compute_estimates_rates_and_branches(VERBOSE, param_tol, all_paths,
                                           th, the_model);

    estimate_root_distribution(all_paths, the_model);

    std::ofstream of;
    if (!outfile.empty()) of.open(outfile.c_str());
    std::ostream out(outfile.empty() ? std::cout.rdbuf() : of.rdbuf());
    if (!out)
      throw std::runtime_error("bad output file: " + outfile);

    out << the_model.format_for_param_file() << endl;
    the_tree.set_branch_lengths(th.branches);
    out << the_tree << endl;
  }
  catch (const std::exception &e) {
    cerr << e.what() << endl;
    return EXIT_FAILURE;
  }
}
