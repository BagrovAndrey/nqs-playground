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

#pragma once

#include "monte_carlo.hpp"

TCM_NAMESPACE_BEGIN

// ------------------------------- [DataSet] ------------------------------- {{{
/// \brief A poor man's alternative to `torch.ConcatDataset`.
///
/// Multiple Markov chains are simply concatenated.
class DataSet {
  private:
    // NOTE: We use shared_ptr's here because we share the ownership of the data
    // with Python code.
    std::vector<std::shared_ptr<ChainResult const>> _chunks;
    std::vector<size_t>                             _cum_sizes;

  public:
    /// Constructs a new dataset from multiple Markov chains.
    DataSet(std::vector<std::shared_ptr<ChainResult const>> chunks);

    DataSet(DataSet const&)     = default;
    DataSet(DataSet&&) noexcept = default;
    DataSet& operator=(DataSet const&) = default;
    DataSet& operator=(DataSet&&) noexcept = default;

    /// Returns the total number of samples in the dataset
    inline auto size() const noexcept -> size_t;

    /// Returns the number of spins in the system.
    ///
    /// It is assumed that all samples have the sample number of spins.
    inline auto number_spins() const noexcept -> size_t;

    /// Returns the `i`'th sample.
    ///
    /// \precondition `i < size()`.
    inline auto operator[](size_t i) const noexcept -> ChainState const&;

    /// Returns the `i`'th sample.
    ///
    /// \throws std::out_of_range if `i >= size()`.
    inline auto at(size_t i) const -> ChainState const&;

  private:
    template <class Iterator>
    auto check_valid(Iterator begin, Iterator end) -> void;
};

auto DataSet::size() const noexcept -> size_t { return _cum_sizes.back(); }

auto DataSet::number_spins() const noexcept -> size_t
{
    TCM_ASSERT(!_chunks.empty(), "number of chunks must be >0 by construction");
    return _chunks[0]->number_spins();
}

auto DataSet::operator[](size_t const i) const noexcept -> ChainState const&
{
    TCM_ASSERT(
        i < size(),
        noexcept_format("index out of bounds: {}; expected <={}", i, size()));
    if (_chunks.size() == 1) { return _chunks[0]->samples()[i]; }
    //
    // indices: [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15]
    //           ^^^^^^^^^       ^            ^^^^^^^^
    // sizes:        5     ^^^^^ 1 ^^^^^^^^^^     3
    //                       3         4
    // cum_sizes:    5       8   9    13          16
    //                                  ^
    //                                  |
    //                                (3, 2)
    //
    // So if we want to get the 11'th sample, we first find the upper bound for
    // 11 in the _cum_sizes vector. This gives us chunk_index == 3. Then, to get
    // the inner index in the third chunk, we subtract _cum_sizes[chunk - 1]
    // (i.e. 9). Thus we arrive at: 2nd element in the 3rd chunk.
    auto const chunk_index = static_cast<size_t>(std::distance(
        std::begin(_cum_sizes),
        std::upper_bound(std::begin(_cum_sizes), std::end(_cum_sizes), i)));
    TCM_ASSERT(chunk_index < _chunks.size(), "");
    TCM_ASSERT(chunk_index == 0
                   || ((i >= _cum_sizes[chunk_index - 1])
                       && (i - _cum_sizes[chunk_index - 1]
                           < _chunks[chunk_index]->samples().size())),
               noexcept_format(
                   "i = {}, chunk_index = {}, _cum_sizes[chunk_index - 1] = "
                   "{}, _chunks[chunk_index]->samples().size() = {}",
                   i, chunk_index, _cum_sizes[chunk_index - 1],
                   _chunks[chunk_index]->samples().size()));
    return (chunk_index == 0)
               ? _chunks[0]->samples()[i]
               : _chunks[chunk_index]
                     ->samples()[i - _cum_sizes[chunk_index - 1]];
}

auto DataSet::at(size_t const i) const -> ChainState const&
{
    TCM_CHECK(i < size(), std::out_of_range,
              fmt::format("index out of range: {}; expected <{}", i, size()));
    return (*this)[i];
}
// ------------------------------- [DataSet] ------------------------------- }}}

// ---------------------------- [IndexSampler] ----------------------------- {{{
/// Iterates over batches of indices.
class IndexSampler {
  private:
    /// All the indices of the data samples.
    std::vector<unsigned> _indices;
    /// Our current position in the `_indices` vector.
    size_t _index;
    /// Size of a batch of indices.
    size_t _batch_size;
    /// Whether to shuffle the indices.
    bool _shuffle;
    /// In case `_indices.size() % _batch_size != 0` the last chunk will be
    /// smaller than `_batch_size`. This parameter decides whether we should
    /// ignore the smaller chunk altogether.
    bool _ignore_last;

  public:
    /// Constructs a new sampler.
    ///
    /// \param size        Number of samples.
    /// \param batch_size  The desired batch size.
    /// \param shuffle     If true, indices will be shuffled on every #reset.
    ///                    Otherwise, indices will be processed in order.
    /// \param ignore_last If true, the last, smaller, batch will be ignored.
    IndexSampler(size_t size, size_t batch_size, bool shuffle,
                 bool ignore_last);

    IndexSampler(IndexSampler const&)     = default;
    IndexSampler(IndexSampler&&) noexcept = default;
    IndexSampler& operator=(IndexSampler const&) = default;
    IndexSampler& operator=(IndexSampler&&) noexcept = default;

    constexpr auto batch_size() const noexcept -> size_t { return _batch_size; }
    constexpr auto shuffle() const noexcept -> bool { return _shuffle; }
    constexpr auto ignore_last() const noexcept -> bool { return _ignore_last; }

    /// Resets the sampler.
    auto reset() -> void;

    /// Returns the next batch of indices.
    inline auto next() noexcept -> gsl::span<unsigned const>;
};

auto IndexSampler::next() noexcept -> gsl::span<unsigned const>
{
    TCM_ASSERT(_index <= _indices.size(),
               noexcept_format("{} > {}", _index, _indices.size()));
    auto const remaining_indices = _indices.size() - _index;
    if (remaining_indices == 0
        || (_ignore_last && remaining_indices < _batch_size)) {
        return {};
    }
    auto const size = std::min(remaining_indices, _batch_size);
    auto const result =
        gsl::span<unsigned const>{_indices.data() + _index, size};
    _index += size;
    TCM_ASSERT(
        _index <= _indices.size(),
        noexcept_format("{} > {}; size = ", _index, _indices.size(), size));
    return result;
}
// ---------------------------- [IndexSampler] ----------------------------- }}}

// ----------------------------- [DataLoader] ------------------------------ {{{
class DataLoader {
  public:
    /// Types of transformations which can be applied to values.
    ///
    /// Using #Transform::Amplitude will result in applying `|.|` operation to
    /// all values. #Transform::Sign will result in applying the `signum`
    /// function to all the values. Moreover, the values will be returned as a
    /// tensor of `int64_t` rather than `float` so that they can be directly
    /// used to train a classifier.
    enum class Transform { No, Amplitude, Sign };

    /// An example on which to train.
    struct Example {
        /// Inputs
        torch::Tensor spins;
        /// Outputs
        torch::Tensor values;
        /// Number of times each value in `spins` was encountered during Monte
        /// Carlo sampling.
        ///
        /// \note It is currently not used anywhere but included here for
        /// completeness.
        torch::Tensor counts;

        Example()                   = default;
        Example(Example const&)     = default;
        Example(Example&&) noexcept = default;
        Example& operator=(Example const&) = default;
        Example& operator=(Example&&) noexcept = default;

        /// Allocates tensors for \p batch_size samples with system size
        /// `number_spins`. The type of the #values tensor is deduced from
        /// \p transform.
        inline Example(size_t batch_size, size_t number_spins,
                       DataLoader::Transform transform);

        inline Example(std::tuple<torch::Tensor, torch::Tensor, torch::Tensor>);
        Example(torch::Tensor s, torch::Tensor v, torch::Tensor c);

        /// Slices the sample along the zeroth dimension.
        inline auto slice(size_t first, size_t last) const -> Example;

        /// Compares two #Examples for equality. However, examples are not
        /// compared in the mathematical sense. Rather, it is checked whether
        /// examples "point" to the same position in the dataset. Obviously,
        /// this means that #kind_of_equal will only work correctly on examples
        /// from the same dataset, but it's only used by #Iterator, so that
        /// should be fine.
        friend inline auto kind_of_equal(Example const& x, Example const& y)
            -> bool;

      private:
        inline Example(UnsafeTag, torch::Tensor s, torch::Tensor v, torch::Tensor c);
    };

    struct Iterator;

    /// Marks the end of iteration.
    struct Sentinel {
        constexpr auto operator==(Sentinel const& other) noexcept -> bool;
        constexpr auto operator!=(Sentinel const& other) noexcept -> bool;
        constexpr auto operator==(Iterator const& other) noexcept -> bool;
        constexpr auto operator!=(Iterator const& other) noexcept -> bool;
    };

    struct Iterator {
        using value_type        = Example;
        using reference         = Example const&;
        using pointer           = Example const*;
        using iterator_category = std::input_iterator_tag;

        friend class DataLoader;

      private:
        DataLoader*    _loader;
        Example const* _batch;

      public:
        constexpr Iterator() noexcept : _loader{nullptr}, _batch{nullptr} {}
        constexpr Iterator(Iterator const&) noexcept = default;
        constexpr Iterator(Iterator&&) noexcept      = default;
        constexpr Iterator& operator=(Iterator const&) noexcept = default;
        constexpr Iterator& operator=(Iterator&&) noexcept = default;
        inline auto         operator++() -> Iterator&;
        inline auto         operator++(int);
        constexpr auto      operator*() const noexcept -> reference;
        constexpr auto      operator-> () const noexcept -> pointer;
        constexpr auto      operator==(Iterator const& other) const -> bool;
        constexpr auto      operator!=(Iterator const& other) const -> bool;
        constexpr auto operator==(Sentinel const& other) const noexcept -> bool;
        constexpr auto operator!=(Sentinel const& other) const noexcept -> bool;

      private:
        constexpr Iterator(DataLoader& loader, Example const* batch) noexcept;
    };

  private:
    DataSet                        _dataset;
    IndexSampler                   _sampler;
    Example                        _batch;
    std::vector<ChainState const*> _temp_buffer;
    Transform                      _transform;

  public:
    DataLoader(DataSet&& dataset, IndexSampler&& sampler, Transform transform);

    DataLoader(DataLoader const&) = delete;
    DataLoader(DataLoader&&)      = delete;
    DataLoader& operator=(DataLoader const&) = delete;
    DataLoader& operator=(DataLoader&&) = delete;

    auto reset() -> void;
    auto next() -> Example const*;

    inline auto begin() -> Iterator;
    inline auto end() -> Iterator;
};

auto DataLoader::begin() -> Iterator { return {*this, next()}; }

auto DataLoader::end() -> Iterator { return {*this, nullptr}; }

// ------------------------------ [Example] -------------------------------- {{{
DataLoader::Example::Example(size_t const batch_size, size_t const number_spins,
                             DataLoader::Transform const transform)
    : spins{detail::make_tensor<float>(batch_size, number_spins)}
    , values{transform == DataLoader::Transform::Sign
                 ? detail::make_tensor<int64_t>(batch_size)
                 : detail::make_tensor<float>(batch_size)}
    , counts{detail::make_tensor<int64_t>(batch_size)}
{}

DataLoader::Example::Example(UnsafeTag, torch::Tensor s, torch::Tensor v,
                             torch::Tensor c)
    : spins{std::move(s)}, values{std::move(v)}, counts{std::move(c)}
{
    TCM_ASSERT(spins.defined(), "");
    TCM_ASSERT(values.defined(), "");
    TCM_ASSERT(counts.defined(), "");
    TCM_ASSERT(spins.dim() == 2 && values.dim() == 1 && counts.dim() == 1, "");
    TCM_ASSERT(
        spins.size(0) == values.size(0) && spins.size(0) == counts.size(0), "");
    TCM_ASSERT(spins.scalar_type() == torch::kFloat32
               && counts.scalar_type() == torch::kInt64
               && (values.scalar_type() == torch::kFloat32
                   || values.scalar_type() == torch::kInt64), "");
}

DataLoader::Example::Example(
    std::tuple<torch::Tensor, torch::Tensor, torch::Tensor> tensors)
    : Example{std::move(std::get<0>(tensors)), std::move(std::get<1>(tensors)),
              std::move(std::get<2>(tensors))}
{}

auto DataLoader::Example::slice(size_t const first, size_t const last) const
    -> Example
{
    TCM_ASSERT(spins.defined(), "");
    TCM_ASSERT(values.defined(), "");
    TCM_ASSERT(counts.defined(), "");
    auto const b = static_cast<int64_t>(first);
    auto const e = static_cast<int64_t>(last);
    return Example{UnsafeTag{}, spins.slice(/*dim=*/0, b, e),
                   values.slice(/*dim=*/0, b, e),
                   counts.slice(/*dim=*/0, b, e)};
}

auto kind_of_equal(DataLoader::Example const& x, DataLoader::Example const& y)
    -> bool
{
    if (x.spins.data_ptr() == y.spins.data_ptr()) {
        TCM_ASSERT(x.values.data_ptr() == y.values.data_ptr(), "");
        TCM_ASSERT(x.counts.data_ptr() == y.counts.data_ptr(), "");
        return true;
    }
    return false;
}
// ------------------------------ [Example] -------------------------------- }}}

// ------------------------------ [Sentinel] ------------------------------- {{{
constexpr auto DataLoader::Sentinel::operator==(Sentinel const& other) noexcept
    -> bool
{
    return true;
}

constexpr auto DataLoader::Sentinel::operator!=(Sentinel const& other) noexcept
    -> bool
{
    return false;
}

constexpr auto DataLoader::Sentinel::operator==(Iterator const& other) noexcept
    -> bool
{
    return other == *this;
}

constexpr auto DataLoader::Sentinel::operator!=(Iterator const& other) noexcept
    -> bool
{
    return other != *this;
}
// ------------------------------ [Sentinel] ------------------------------- }}}

// ------------------------------ [Iterator] ------------------------------- {{{
constexpr DataLoader::Iterator::Iterator(DataLoader&    loader,
                                         Example const* batch) noexcept
    : _loader{&loader}, _batch{batch}
{}

auto DataLoader::Iterator::operator++() -> Iterator&
{
    TCM_ASSERT(_loader != nullptr && _batch != nullptr,
               "iterator not incrementable");
    _batch = _loader->next();
    return *this;
}

auto DataLoader::Iterator::operator++(int)
{
    TCM_ASSERT(_loader != nullptr && _batch != nullptr,
               "iterator not incrementable");
    // NOTE(twesterhout): This is a hack.
    //
    // LegacyInputIterator concept requires that for an lvalue `x`
    // `(void)x++` should be equivalent to `(void)++x`. This can be done
    // by simply returning `void` in `operator++(int)`. However, there is
    // another requirement that says that `*x++` should be equivalent to
    // `value_type t = *x; ++x; return t`. So we define a wrapper type
    // with a single operation: `operator*` which holds `t`.
    struct Wrapper {
        value_type x;

        Wrapper(value_type const& value) : x{value} {}
        Wrapper(Wrapper const&) = delete;
        Wrapper(Wrapper&&) =
            default; // Before C++17 we need this to return by value
        Wrapper& operator=(Wrapper const&) = delete;
        Wrapper& operator=(Wrapper&&) = delete;

        auto     operator*()
            && noexcept(std::is_nothrow_move_constructible<value_type>::value)
                   -> value_type
        {
            return std::move(x);
        }
    };

    Wrapper wrapper{*_batch};
    ++(*this);
    return wrapper;
}

constexpr auto DataLoader::Iterator::operator*() const noexcept -> reference
{
    TCM_ASSERT(_batch != nullptr, "iterator not dereferenceable");
    return *_batch;
}

constexpr auto DataLoader::Iterator::operator-> () const noexcept -> pointer
{
    TCM_ASSERT(_batch != nullptr, "iterator not dereferenceable");
    return _batch;
}

constexpr auto DataLoader::Iterator::operator==(Iterator const& other) const
    -> bool
{
    TCM_ASSERT(_loader != nullptr && _loader == other._loader,
               "iterators pointing to different DataLoader's cannot be "
               "compared");
    // Very rarely do we compare two iterators where neither one of them is
    // the one-past-the-end iterator.
    if (TCM_UNLIKELY(_batch != nullptr && other._batch != nullptr)) {
        return kind_of_equal(*_batch, *other._batch);
    }
    return _batch == nullptr && other._batch == nullptr;
}

constexpr auto DataLoader::Iterator::operator!=(Iterator const& other) const
    -> bool
{
    return !(*this == other);
}

constexpr auto DataLoader::Iterator::operator==(Sentinel const& other) const
    noexcept -> bool
{
    TCM_ASSERT(_loader != nullptr, "iterators are not comparable");
    return _batch == nullptr;
}

constexpr auto DataLoader::Iterator::operator!=(Sentinel const& other) const
    noexcept -> bool
{
    return !(*this == other);
}
// ------------------------------ [Iterator] ------------------------------- }}}
// ----------------------------- [DataLoader] ------------------------------ }}}

auto bind_dataloader(pybind11::module m) -> void;

TCM_NAMESPACE_END

// We make Python think that our `tcm::DataLoader::Example`s are just tuples.
// It's nice, because one can then write loops like
// ```{.py}
// for x, y, _ in dataloader:
//     ... loss(module(x), y) ...
// ```
namespace pybind11 {
namespace detail {
    template <> struct type_caster<::TCM_NAMESPACE::DataLoader::Example> {
      public:
        PYBIND11_TYPE_CASTER(::TCM_NAMESPACE::DataLoader::Example,
                             _("Example"));

        auto load(handle src, bool convert) -> bool
        {
            using TupleT =
                std::tuple<torch::Tensor, torch::Tensor, torch::Tensor>;
            type_caster<TupleT> caster;
            if (!caster.load(src, convert)) { return false; }
            value = ::TCM_NAMESPACE::DataLoader::Example{
                static_cast<TupleT&&>(std::move(caster))};
            return true;
        }

        static handle cast(::TCM_NAMESPACE::DataLoader::Example src,
                           return_value_policy /* policy */,
                           handle /* parent */)
        {
            return py::cast(std::make_tuple(src.spins, src.values, src.counts))
                .release();
        }
    };
} // namespace detail
} // namespace pybind11
