#include "unpack.hpp"

#include "../errors.hpp"
#include <vectorclass/version2/vectorclass.h>

TCM_NAMESPACE_BEGIN
namespace cpu {

// unpack {{{
namespace detail {
    struct _IdentityProjection {
        template <class T>
        constexpr decltype(auto) operator()(T&& x) const noexcept
        {
            return std::forward<T>(x);
        }
    };

    TCM_FORCEINLINE auto _unpack(uint8_t const bits) noexcept -> vcl::Vec8f
    {
        auto const one = vcl::Vec8f{1.0f}; // 1.0f == 0x3f800000
        auto const two = vcl::Vec8f{2.0f};
        // Adding 0x3f800000 to select ensures that we're working with valid
        // floats rather than denormals
        auto const select = vcl::Vec8f{vcl::reinterpret_f(vcl::Vec8i{
            0x3f800000 + (1 << 0), 0x3f800000 + (1 << 1), 0x3f800000 + (1 << 2),
            0x3f800000 + (1 << 3), 0x3f800000 + (1 << 4), 0x3f800000 + (1 << 5),
            0x3f800000 + (1 << 6), 0x3f800000 + (1 << 7)})};
        auto       broadcasted =
            vcl::Vec8f{vcl::reinterpret_f(vcl::Vec8i{static_cast<int>(bits)})};
        broadcasted |= one;
        broadcasted &= select;
        broadcasted = broadcasted == select;
        broadcasted &= two;
        broadcasted -= one;
        return broadcasted;
    }

    TCM_FORCEINLINE auto _unpack_word(uint64_t x, float* out) TCM_NOEXCEPT
        -> float*
    {
        for (auto i = 0; i < 8; ++i, out += 8, x >>= 8U) {
            _unpack(static_cast<uint8_t>(x & 0xFF)).store(out);
        }
        return out;
    }

    template <bool Unsafe>
    TCM_FORCEINLINE auto _unpack(uint64_t x, unsigned const number_spins,
                                 float* out) TCM_NOEXCEPT -> float*
    {
        auto const chunks = number_spins / 8U;
        auto const rest   = number_spins % 8U;
        auto const y      = x; // Only for testing
        for (auto i = 0U; i < chunks; ++i, out += 8, x >>= 8U) {
            _unpack(static_cast<uint8_t>(x & 0xFF)).store(out);
        }
        if (rest != 0) {
            auto const t = _unpack(static_cast<uint8_t>(x & 0xFF));
            if constexpr (Unsafe) { t.store(out); }
            else {
                t.store_partial(static_cast<int>(rest), out);
            }
            out += rest;
        }

        TCM_ASSERT(
            ([y, number_spins, out]() {
                auto* p = out - static_cast<ptrdiff_t>(number_spins);
                for (auto i = 0U; i < number_spins; ++i) {
                    if (!((p[i] == 1.0f && ((y >> i) & 0x01) == 0x01)
                          || (p[i] == -1.0f && ((y >> i) & 0x01) == 0x00))) {
                        return false;
                    }
                }
                return true;
            }()),
            noexcept_format(
                "{} vs [{}]", y,
                fmt::join(out - static_cast<ptrdiff_t>(number_spins), out,
                          ", ")));
        return out;
    }

    template <bool Unsafe>
    TCM_FORCEINLINE auto _unpack(bits512 const& bits, unsigned const count,
                                 float* out) TCM_NOEXCEPT -> float*
    {
        constexpr auto block = 64U;

        auto i = 0U;
        for (; i < count / block; ++i) {
            out = _unpack_word(bits.words[i], out);
        }
        if (auto const rest = count % block; rest != 0) {
            out = _unpack<Unsafe>(bits.words[i], rest, out);
        }
        return out;
    }
} // namespace detail

template <class Bits, class Projection = detail::_IdentityProjection>
auto unpack_impl(TensorInfo<Bits const> const& src_info,
                 TensorInfo<float, 2> const&   dst_info,
                 Projection                    proj = Projection{}) -> void
{
    if (TCM_UNLIKELY(src_info.size() == 0)) { return; }
    TCM_CHECK(
        dst_info.strides[0] == dst_info.sizes[1], std::invalid_argument,
        fmt::format("unpack_cpu_avx does not support strided output tensors"));
    auto const number_spins = static_cast<unsigned>(dst_info.sizes[1]);
    auto const rest         = number_spins % 8;
    auto const tail         = std::min<int64_t>(
        ((8U - rest) + number_spins - 1U) / number_spins, src_info.size());
    auto* src = src_info.data;
    auto* dst = dst_info.data;
    auto  i   = int64_t{0};
    for (; i < src_info.size() - tail;
         ++i, src += src_info.stride(), dst += dst_info.strides[0]) {
        detail::_unpack</*Unsafe=*/true>(proj(*src), number_spins, dst);
    }
    for (; i < src_info.size();
         ++i, src += src_info.stride(), dst += dst_info.strides[0]) {
        detail::_unpack</*Unsafe=*/false>(proj(*src), number_spins, dst);
    }
}

#if 0
template <class Iterator, class Sentinel,
          class Projection = detail::_IdentityProjection>
auto unpack(Iterator first, Sentinel last, unsigned const number_spins,
            torch::Tensor& dst, Projection proj = Projection{}) -> void
{
    if (first == last) { return; }
    auto const size = static_cast<size_t>(std::distance(first, last));
    auto const rest = number_spins % 8;
    auto const tail = std::min<size_t>(
        ((8U - rest) + number_spins - 1U) / number_spins, size);
    auto* data = dst.data_ptr<float>();
    for (auto i = size_t{0}; i < size - tail; ++i, ++first) {
        data =
            detail::_unpack</*Unsafe=*/true>(proj(*first), number_spins, data);
    }
    for (auto i = size - tail; i < size; ++i, ++first) {
        data =
            detail::_unpack</*Unsafe=*/false>(proj(*first), number_spins, data);
    }
}
#endif
// }}}

template <class Bits>
auto unpack_cpu_avx(TensorInfo<Bits const> const& src_info,
                    TensorInfo<float, 2> const&   dst_info) -> void
{
    unpack_impl(src_info, dst_info);
}

template TCM_EXPORT auto unpack_cpu_avx(TensorInfo<uint64_t const> const&,
                                        TensorInfo<float, 2> const&) -> void;
template TCM_EXPORT auto unpack_cpu_avx(TensorInfo<bits512 const> const&,
                                        TensorInfo<float, 2> const&) -> void;

#if 0
TCM_EXPORT auto unpack_cpu_avx(torch::Tensor spins, int64_t const number_spins,
                               torch::Tensor out) -> void
{
    auto const first = static_cast<uint64_t const*>(spins.data_ptr());
    auto const last  = first + spins.size(0);
    unpack(first, last, static_cast<unsigned>(number_spins), out);
}
#endif

#if 0
TCM_EXPORT auto unpack(torch::Tensor x, torch::Tensor indices,
                       int64_t const number_spins) -> torch::Tensor
{
    TCM_CHECK(
        number_spins > 0, std::invalid_argument,
        fmt::format("invalid number_spins: {}; expected a positive integer",
                    number_spins));
    TCM_CHECK_TYPE("x", x, torch::kInt64);
    TCM_CHECK_TYPE("indices", indices, torch::kInt64);
    TCM_CHECK_CONTIGUOUS("x", x);

    auto x_shape = x.sizes();
    TCM_CHECK(x_shape.size() == 1 || (x_shape.size() == 2 && x_shape[1] == 1),
              std::domain_error,
              fmt::format("x has wrong shape: [{}]; expected a vector",
                          fmt::join(x_shape, ", ")));
    auto indices_shape = indices.sizes();
    TCM_CHECK(indices_shape.size() == 1
                  || (indices_shape.size() == 2 && indices_shape[1] == 1),
              std::domain_error,
              fmt::format("indices has wrong shape: [{}]; expected a vector",
                          fmt::join(indices_shape, ", ")));

    auto out = torch::empty(
        std::initializer_list<int64_t>{indices_shape[0], number_spins},
        torch::TensorOptions{}.dtype(torch::kFloat32));
    auto const first = static_cast<int64_t const*>(indices.data_ptr());
    auto const last  = first + indices_shape[0];
    unpack(first, last, static_cast<unsigned>(number_spins), out,
           [data = static_cast<uint64_t const*>(x.data_ptr()),
            n    = x_shape[0]](auto const i) {
               TCM_CHECK(
                   0 <= i && i < n, std::out_of_range,
                   fmt::format("indices contains an index which is out of "
                               "range: {}; expected an index in [{}, {})",
                               i, 0, n));
               return data[i];
           });
    return out;
}
#endif

} // namespace cpu
TCM_NAMESPACE_END
