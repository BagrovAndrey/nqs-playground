// Copyright (c) 2019, Tom Westerhout
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "nqs.hpp"
#include <pybind11/pybind11.h>
#include <torch/extension.h>

namespace py = pybind11;

namespace {
auto bind_polynomial_state(py::module m) -> void
{
    using namespace tcm;

    py::class_<PolynomialStateV2>(m, "PolynomialState")
        .def(py::init([](std::shared_ptr<Polynomial> polynomial,
                         std::string const&          state,
                         std::pair<size_t, size_t>   input_shape) {
            return std::make_unique<PolynomialStateV2>(
                std::move(polynomial), load_forward_fn(state), input_shape);
        }))
        .def("__call__", [](PolynomialStateV2&                          self,
                            py::array_t<SpinVector, py::array::c_style> spins) {
            return self({spins.data(0), static_cast<size_t>(spins.shape(0))});
        });
}
} // namespace


#if defined(TCM_CLANG)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wmissing-prototypes"
#endif
PYBIND11_MODULE(_C_nqs, m)
{
#if defined(TCM_CLANG)
#    pragma clang diagnostic pop
#endif
    m.doc() = R"EOF()EOF";

    using namespace tcm;

    bind_spin(m.ptr());
    bind_heisenberg(m);
    bind_explicit_state(m);
    bind_polynomial(m);
    // bind_options(m);
    // bind_chain_result(m);
    // bind_sampling(m);
    // bind_networks(m);
    // bind_dataloader(m);
    bind_monte_carlo(m.ptr());
    bind_polynomial_state(m);
}
