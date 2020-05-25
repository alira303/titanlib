#include "titanlib.h"

std::string titanlib::version() {
    return __version__;
}

float* titanlib::test_array(float* v, int n) {
    int count = 0;
    for(int i = 0; i < n; i++)
        count++;
    return v;
 }

void titanlib::initialize_omp() {
#ifdef _OPENMP
    int num_threads = 1;
    const char* num_threads_char = std::getenv("OMP_NUM_THREADS");
    if(num_threads_char != NULL) {
        std::istringstream(std::string(num_threads_char)) >> num_threads;
        if(num_threads <= 0)
            num_threads = 1;
    }
    titanlib::set_omp_threads(num_threads);
#endif
}
void titanlib::set_omp_threads(int num) {
#ifdef _OPENMP
    // omp_set_dynamic(0);
    omp_set_num_threads(num);
#endif
}