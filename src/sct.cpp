#include <vector>
#include <math.h>
#include "titanlib.h"
#include <assert.h>
#include <iostream>
#include <numeric>
#include <exception>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_linalg.h>
#include <boost/numeric/ublas/vector.hpp>
#include <boost/numeric/ublas/vector_proxy.hpp>
#include <boost/numeric/ublas/triangular.hpp>
#include <boost/numeric/ublas/lu.hpp>
#include <boost/numeric/ublas/io.hpp>
#include <boost/numeric/ublas/matrix.hpp>


// helpers
bool invert_matrix (const boost::numeric::ublas::matrix<float>& input, boost::numeric::ublas::matrix<float>& inverse);

// forward declarations
fvec compute_vertical_profile(const fvec& lats, const fvec& lons, const fvec& elevs, const fvec& values, double meanT, double gamma, double a, double exact_p10, double exact_p90, int nminprof, double dzmin);
double basic_vertical_profile_optimizer_function(const gsl_vector *v, void *data);
fvec basic_vertical_profile(const int n, const double *elevs, const double t0, const double gamma);
double vertical_profile_optimizer_function(const gsl_vector *v, void *data);
fvec vertical_profile(const int n, const double *elevs, const double t0, const double gamma, const double a, const double h0, const double h1i);

// start SCT //
ivec titanlib::sct(const fvec& lats,
        const fvec& lons,
        const fvec& elevs,
        const fvec& values,
        // determine if we have too many or too few observations
        // (too many means we can reduce the distance, too few mean isolation problem and cannot flag?)
        int minnumobs,
        int maxnumobs,
        // first find everything close to the point that we are testing (maxdist)
        double inner_radius,
        double outer_radius,
        int niterations,
        int nminprof,
        double dzmin,
        double dhmin,
        float dz,
        const fvec& t2pos,
        const fvec& t2neg,
        const fvec& eps2,
        fvec& sct,
        fvec& rep) {

    const int s = values.size();
    // assert that the arrays we expect are of size s
    if( lats.size() != s || lons.size() != s || elevs.size() != s || values.size() != s) {
        throw std::runtime_error("Dimension mismatch");
    }

    ivec flags(s, 0); // TODO: should this be the size of the full box, or the size of the part of the box we are flagging 
    sct.clear();
    sct.resize(s, 0);
    rep.clear();
    rep.resize(s, 0);

    // create the KD tree
    titanlib::KDTree tree(lats, lons);

    for(int iteration = 0; iteration < niterations; iteration++) {

        int thrownOut = 0; // reset this number each loop (this is for breaking if we don't throw anything new out)

        // loop over all observations
        for(int curr=0; curr < s; curr++) {
            // break out if station already flagged
            if(flags[curr] != 0)
                continue;
            // get all neighbours that are close enough
            ivec neighbour_indices = tree.get_neighbours(lats[curr], lons[curr], inner_radius, maxnumobs, false);
            if(neighbour_indices.size() < minnumobs) {
                // flag as isolated? 
                continue; // go to next station, skip this one
            }
            ivec neighbour_indices_removed_flagged;
            neighbour_indices_removed_flagged.reserve(neighbour_indices.size());
            for(int i=0; i<neighbour_indices.size(); i++) {
                if(flags[neighbour_indices[i]] == 0 ) {
                    neighbour_indices_removed_flagged.push_back(neighbour_indices[i]);
                }
            }
            if(neighbour_indices_removed_flagged.size() < minnumobs) {
                // flag as isolated? 
                continue; // go to next station, skip this one
            }

            // add the actual station at the end
            neighbour_indices_removed_flagged.push_back(curr);

            // call SCT with this box 
            fvec lons_box = titanlib::util::subset(lons, neighbour_indices_removed_flagged);
            fvec elevs_box = titanlib::util::subset(elevs, neighbour_indices_removed_flagged);
            fvec lats_box = titanlib::util::subset(lats, neighbour_indices_removed_flagged);
            fvec values_box = titanlib::util::subset(values, neighbour_indices_removed_flagged);
            fvec eps2_box = titanlib::util::subset(eps2, neighbour_indices_removed_flagged);
            int s_box = neighbour_indices_removed_flagged.size();
            // the thing to flag is at "curr", ano not included in the box

            /*
               Stuff for VP
               */
            double gamma = -0.0065;
            double a = 5.0;
            double meanT = std::accumulate(values_box.begin(), values_box.end(), 0.0) / s;
            // std::cout << meanT << std::endl;
            double exact_p10 = titanlib::util::compute_quantile(0.10, elevs_box);
            double exact_p90 = titanlib::util::compute_quantile(0.90, elevs_box);

            // calculate background
            fvec vp = compute_vertical_profile(lats_box, lons_box, elevs_box, values_box, meanT, gamma, a, exact_p10, exact_p90, nminprof, dzmin);
            // now have temperature profile (vp)
            for(int i=0; i < s_box; i++) {
                assert(vp[i] !=-999);
            }

            boost::numeric::ublas::matrix<float> disth(s, s);
            boost::numeric::ublas::matrix<float> distz(s, s);
            boost::numeric::ublas::vector<float> Dh(s);

            for(int i=0; i < s_box; i++) {
                fvec Dh_vector(s_box);
                for(int j=0; j < s_box; j++) {
                    disth(i, j) = titanlib::util::calc_distance(lats_box[i], lons_box[i], lats_box[j], lons_box[j]);
                    distz(i, j) = fabs(elevs_box[i] - elevs_box[j]);
                    if(i != j) {
                        if(i < j)
                            Dh_vector[j - 1] = disth(i, j);
                        else if(i > j)
                            Dh_vector[j] = disth(i, j);
                    }
                }
                Dh(i) = titanlib::util::compute_quantile(0.10, Dh_vector);
            }

            double Dh_mean = std::accumulate(std::begin(Dh), std::end(Dh), 0.0) / Dh.size();
            if(Dh_mean < dhmin) {
                Dh_mean = dhmin;
            }

            boost::numeric::ublas::matrix<float> S(s_box,s_box);
            boost::numeric::ublas::matrix<float> Sinv(s_box,s_box);
            for(int i=0; i < s_box; i++) {
                for(int j=0; j < s_box; j++) {
                    double value = std::exp(-.5 * std::pow((disth(i, j) / Dh_mean), 2) - .5 * std::pow((distz(i, j) / dz), 2));
                    if(i==j) { // weight the diagonal?? (0.5 default)
                        value = value + eps2_box[i];
                    }
                    S(i,j) = value;
                }
            }

            boost::numeric::ublas::vector<float> d(s_box);
            for(int i=0; i < s_box; i++) {
                d(i) = values_box[i] - vp[i]; // difference between actual temp and temperature from vertical profile
            }

            /* ---------------------------------------------------
               Beginning of real SCT
               ------------------------------------------------------*/
            bool b = invert_matrix(S, Sinv);
            if(b != true) {
                // TODO: flag differently or give an error???
                continue;
            }

            boost::numeric::ublas::vector<float> Zinv(s_box), Sinv_d(s_box), ares_temp(s_box), ares(s_box);
            for(int i=0; i<s_box; i++) {
                double acc = 0;
                for(int j=0; j<s_box; j++) {
                    acc += Sinv(i,j)*d(j);
                }
                Sinv_d(i) = acc;
            }
            for(int i=0; i<s_box; i++) {
                double acc = 0;
                for(int j=0; j<s_box; j++) {
                    acc += S(i,j)*Sinv_d(j);
                }
                ares_temp(i) = acc;
            }
            for(int i=0; i<s_box; i++) {
                Zinv(i) = (1/Sinv(i,i)); //Zinv<-1/diag(SRinv)
                ares(i) = ares_temp(i)-d(i); // ares<-crossprod(S,SRinv.d)-d[sel]
            }

            boost::numeric::ublas::vector<float> cvres(s_box);
            for(int i=0; i<s_box; i++) {
                cvres(i) = -1*Zinv(i) * Sinv_d(i);
            }

            double sig2o = 0;
            boost::numeric::ublas::vector<float> sig2o_temp(s_box), negAres_temp(s_box);
            for(int i=0; i<s_box; i++) {
                negAres_temp(i)=-1*ares(i);
                sig2o_temp(i) = d(i)*negAres_temp(i);
                sig2o += sig2o_temp(i);
            }

            sig2o = sig2o/s_box;
            if(sig2o < 0.01) {
                sig2o = 0.01;
            }

            // boost::numeric::ublas::vector<float> pog(s_box);
            // for(int i=0; i<s_box; i++) {
            //     pog(i) = cvres(i)*ares(i) / sig2o;
            // }
            float last = s_box - 1;
            float pog = cvres(last) * ares(last) / sig2o;
            // std::cout << "sig2o: " << sig2o << std::endl;
            sct[curr] = pog;
            if((cvres(last) < 0 && pog > t2pos[curr]) || (cvres(last) >= 0 && pog > t2neg[curr])) {
                flags[curr] = 1;
                thrownOut++;
            }

        }
        if(thrownOut == 0) {
            if(iteration + 1 < niterations)
                std::cout << "Stopping early after " << iteration + 1<< " iterations" << std::endl;
            break;
        }

    }

    return flags;
}
// end SCT //


fvec compute_vertical_profile(const fvec& lats, const fvec& lons, const fvec& elevs, const fvec& values, double meanT, double gamma, double a, double exact_p10, double exact_p90, int nminprof, double dzmin) {

    // optimize inputs for VP (using Nelder-Mead Simplex algorithm)
    const gsl_multimin_fminimizer_type *T = gsl_multimin_fminimizer_nmsimplex2;
    gsl_multimin_fminimizer *s = NULL;
    gsl_vector *ss;
    gsl_multimin_function vp_optim;

    int iter = 0;
    int status;
    double size;

    // data (params) that needs to be passed into vp
    double nd = (double) elevs.size(); // cast + resize to double
    std::vector<double> delevs(elevs.begin(), elevs.end());
    std::vector<double> dvalues(values.begin(), values.end());
    double * dpelevs = delevs.data();
    double * dpvalues = dvalues.data();
    double * data[3] = {&nd, dpelevs, dpvalues};

    // Check if terrain is too flat
    double z05 = titanlib::util::compute_quantile(0.05, elevs);
    double z95 = titanlib::util::compute_quantile(0.95, elevs);

    // should we use the basic or more complicated vertical profile?
    bool use_basic = elevs.size() < nminprof || (z95 - z05) < dzmin;

    gsl_vector* input;
    if(use_basic) {
        vp_optim.n = 2;
        input = gsl_vector_alloc(vp_optim.n);
        gsl_vector_set(input, 0, meanT);
        gsl_vector_set(input, 1, gamma);
        vp_optim.f = basic_vertical_profile_optimizer_function;
    }
    else {
        vp_optim.n = 5;
        input = gsl_vector_alloc(vp_optim.n);
        gsl_vector_set(input, 0, meanT);
        gsl_vector_set(input, 1, gamma);
        gsl_vector_set(input, 2, a);
        gsl_vector_set(input, 3, exact_p10);
        gsl_vector_set(input, 4, exact_p90);
        vp_optim.f = vertical_profile_optimizer_function;
    }
    ss = gsl_vector_alloc (vp_optim.n);

    gsl_vector_set_all (ss, 1.0);
    vp_optim.params = data;

    s = gsl_multimin_fminimizer_alloc (T, vp_optim.n);
    gsl_multimin_fminimizer_set (s, &vp_optim, input, ss);
    do {
        iter++;
        status = gsl_multimin_fminimizer_iterate(s);

        if (status)
            break;

        size = gsl_multimin_fminimizer_size (s);
        status = gsl_multimin_test_size (size, 1e-2);
    }
    while (status == GSL_CONTINUE && iter < 100);

    // then actually calculate the vertical profile using the minima
    fvec vp;
    if(use_basic) {
        vp = basic_vertical_profile(nd, dpelevs, gsl_vector_get(s->x, 0), gsl_vector_get(s->x, 1));
    }
    else {
        // then actually calculate the vertical profile using the minima
        vp = vertical_profile(nd, dpelevs,  gsl_vector_get(s->x, 0), gsl_vector_get(s->x, 1),
                gsl_vector_get(s->x, 2), gsl_vector_get(s->x, 3), gsl_vector_get(s->x, 4));
    }

    gsl_vector_free(input);
    gsl_vector_free(ss);
    gsl_multimin_fminimizer_free (s);

    return vp;
}

double basic_vertical_profile_optimizer_function(const gsl_vector *v, void *data) 
{
    double **p = (double **)data;
    int n = (int) *p[0]; // is of type double but should be an int
    double *dpelevs = p[1];
    double *dpvalues = p[2];

    // the parameters to mess with
    double meanT = gsl_vector_get(v,0);
    double gamma = gsl_vector_get(v,1);

    // give everything to vp to compute t_out
    fvec t_out = basic_vertical_profile(n, dpelevs, meanT, gamma);
    // RMS
    float total = 0;
    for(int i=0; i<n; i++) {
        total += pow((t_out[i]-dpvalues[i]),2);
    }
    double value = log(pow((total / n),0.5));
    return value;
}

fvec basic_vertical_profile(const int n, const double *elevs, const double t0, const double gamma)
{
    fvec t_out;
    t_out.resize(n, -999);

    for(int i=0; i<n; i++) {
        t_out[i] = t0 + gamma*elevs[i];
    }
    return t_out;
}

double vertical_profile_optimizer_function(const gsl_vector *v, void *data)
{
    double **p = (double **)data;
    int n = (int) *p[0]; // is of type double but should be an int
    double *dpelevs = p[1];
    double *dpvalues = p[2];

    // the parameters to mess with
    double meanT = gsl_vector_get(v,0);
    double gamma = gsl_vector_get(v,1);
    double a = gsl_vector_get(v,2);
    double exact_p10 = gsl_vector_get(v,3);
    double exact_p90 = gsl_vector_get(v,4);

    // give everything to vp to compute t_out
    fvec t_out = vertical_profile(n, dpelevs, meanT, gamma, a, exact_p10, exact_p90);
    // RMS
    double total = 0;
    for(int i=0; i<n; i++) {
        total += pow((t_out[i]-dpvalues[i]),2);
    }
    double value = log(pow((total / n),0.5));
    return value;
}

fvec vertical_profile(const int n, const double *elevs, const double t0, const double gamma, const double a, const double h0, const double h1i)
{
    double h1 = h0 + fabs(h1i); // h1<-h0+abs(h1i)
    // loop over the array of elevations
    fvec t_out;
    t_out.resize(n, -999);

    for(int i=0; i<n; i++) {
        // define some bools
        bool z_le_h0 = elevs[i] <= h0; // z.le.h0<-which(z<=h0)
        bool z_ge_h1 = elevs[i] >= h1; // z.ge.h1<-which(z>=h1)
        bool z_in = (elevs[i]>h0 && elevs[i]<h1); // z.in<-which(z>h0 & z<h1)
        if(z_le_h0) {
            t_out[i] = t0-gamma*elevs[i]-a;
        }
        if(z_ge_h1) {
            t_out[i] = t0-gamma*elevs[i];
        }
        if(z_in) {
            t_out[i] = t0-gamma*elevs[i]-a/2*(1+cos(M_PI*(elevs[i]-h0)/(h1-h0)));
        }
    }
    return t_out;
}


//----------------------------------------------------------------------------//
// HELPER FUNCTIONS

bool invert_matrix(const boost::numeric::ublas::matrix<float>& input, boost::numeric::ublas::matrix<float>& inverse) {
    using namespace boost::numeric::ublas;
    typedef permutation_matrix<std::size_t> pmatrix;
    // create a working copy of the input
    matrix<float> A(input);
    // create a permutation matrix for the LU-factorization
    pmatrix pm(A.size1());
    // perform LU-factorization
    int res = lu_factorize(A,pm);
    if( res != 0 ) return false;
    // create identity matrix of "inverse"
    inverse.assign(boost::numeric::ublas::identity_matrix<float>(A.size1()));
    // backsubstitute to get the inverse
    lu_substitute(A, pm, inverse);
    return true;
}

