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

#include <vector>

void
trans_prob_mat(const double rate0, const double rate1,
               const double time_interval,
               std::vector<std::vector<double> > &transition_matrix);

void
decompose(const std::vector<double> &rates,
          std::vector<double> &eigen_vals,
          std::vector<std::vector<double> > &U,
          std::vector<std::vector<double> > &Uinv);

void
end_cond_sample(const std::vector<double> rates,
                const std::vector<double> eigen_vals,
                const std::vector<std::vector<double> > U,
                const std::vector<std::vector<double> > Uinv,
                const size_t a, const size_t b, const double T,
                std::mt19937 &gen, std::vector<double> &jump_times);

double
end_cond_sample_prob(const std::vector<double> rates,
                     const std::vector<double> eigen_vals,
                     const std::vector<std::vector<double> > U,
                     const std::vector<std::vector<double> > Uinv,
                     size_t a, const size_t b, double T,
                     std::vector<double> jump_times);