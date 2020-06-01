from itertools import islice
import math
from random import randint
from typing import Optional, Tuple, Callable

import numpy as np
import torch
from torch import Tensor

from ._C import MetropolisKernel
from .core import forward_with_batches

__all__ = ["SamplingOptions", "sample_some", "integrated_autocorr_time"]


class SamplingOptions:
    r"""Options for Monte Carlo sampling spin configurations."""

    def __init__(
        self,
        number_samples: int,
        number_chains: int = 1,
        number_discarded: Optional[int] = None,
        sweep_size: int = 1,
        device: torch.device = "cpu",
    ):
        r"""Initialises the options.

        :param number_samples: specifies the number of samples per Markov
            chain. Must be a positive integer.
        :param number_chains: specifies the number of independent Markov
            chains. Must be a positive integer.
        :param number_discarded: specifies the number of samples to discard
            in the beginning of each Markov chain (i.e. how long should the
            thermalisation procedure be). If specified, must be a positive
            integer. Otherwise, 10% of ``number_samples`` is used.
        :param sweep_size:
        :param device:
        """
        self.number_samples = int(number_samples)
        if self.number_samples <= 0:
            raise ValueError("negative number_samples: {}".format(number_samples))
        self.number_chains = int(number_chains)
        if self.number_chains <= 0:
            raise ValueError("negative number_chains: {}".format(number_chains))

        if number_discarded is not None:
            self.number_discarded = int(number_discarded)
            if self.number_discarded <= 0:
                raise ValueError(
                    "invalid number_discarded: {}; expected either a positive "
                    "integer or None".format(number_chains)
                )
        else:
            self.number_discarded = self.number_samples // 10
        self.sweep_size = int(sweep_size)
        if self.sweep_size <= 0:
            raise ValueError("negative sweep_size: {}".format(sweep_size))
        if not isinstance(device, torch.device):
            device = torch.device(device)
        self.device = device


class Sampler:
    r"""Simple and generic sampler which uses Metropolis-Hastings algorithm to
    approximate the target distribution.
    """

    class State:
        def __init__(self, state, norm, log_prob):
            self.state = state
            self.norm = norm
            self.log_prob = log_prob
            self.accepted = torch.zeros(state.size(0), dtype=torch.int64, device=state.device)
            self.steps = 0

        def _step(self, proposed_state, proposed_norm, proposed_log_prob):
            if proposed_log_prob.dim() == 2:
                proposed_log_prob = proposed_log_prob.squeeze(dim=1)
            r = torch.rand(self.state.size(0)) * proposed_norm / self.norm
            t = (proposed_norm > 0) & (r <= torch.exp(proposed_log_prob - self.log_prob))
            self.state[t] = proposed_state[t]
            self.norm[t] = proposed_norm[t]
            self.log_prob[t] = proposed_log_prob[t]
            self.accepted += t
            self.steps += 1

    def __init__(
        self,
        transition_kernel: Callable[[Tensor], Tuple[Tensor, Tensor]],
        log_prob_fn: Callable[[Tensor], Tensor],
        batch_size: int = 32,
        device: Optional[torch.device] = None,
    ):
        r"""Constructs the sampler.

        :param transition_kernel: is a function which generates possible
            transitions. Given the current state ``s`` it should return a new
            state ``s'`` and a so-called norm (basically, just a probability
            correction; TODO: explain it better).
        :param log_prob_fn: is a function which when given a state returns its
            unnormalized log probability.
        :param batch_size: number of Markov chains to generate in parallel.
        """
        if batch_size <= 0:
            raise ValueError(
                "invalid batch_size: {}; expected a positive integer".format(batch_size)
            )
        if device is None:
            device = "cpu"
        if not isinstance(device, torch.device):
            device = torch.device(device)
        self.device = device
        self.kernel = transition_kernel
        self.basis = self.kernel.basis
        self.log_prob_fn = log_prob_fn
        self.batch_size = batch_size
        self._current = None

    def bootstrap(self) -> Tuple[Tensor, Tensor, Tensor]:
        state = _prepare_initial_state(self.basis, self.batch_size)
        norm = self.basis.norm(state)
        if self.device.type != "cpu":
            state = state.to(self.device)
            norm = norm.to(self.device)
        log_prob = self.log_prob_fn(state)
        if log_prob.dim() == 2:
            log_prob = log_prob.squeeze(dim=1)
        return Sampler.State(state, norm, log_prob)

    def __iter__(self):
        self._current = self.bootstrap()
        while True:
            yield self._current
            proposed_state, proposed_norm = self.kernel(self._current.state)
            proposed_log_prob = self.log_prob_fn(proposed_state)
            self._current._step(proposed_state, proposed_norm, proposed_log_prob)

    @property
    def acceptance_rate(self):
        return self._current.accepted.to(dtype=torch.float64, device="cpu") / self._current.steps


def _sample_using_metropolis(log_ψ: Callable[[Tensor], Tensor], basis, options: SamplingOptions):
    kernel = MetropolisKernel(basis)
    sampler = Sampler(
        kernel, lambda x: 2 * log_ψ(x), batch_size=options.number_chains, device=options.device,
    )

    shape = (options.number_samples, options.number_chains)
    states = torch.empty(shape + (8,), dtype=torch.int64, device=options.device)
    log_prob = torch.empty(shape, dtype=torch.float32, device=options.device)
    assert states.is_contiguous() and log_prob.is_contiguous()
    sweep_size = options.sweep_size if options.sweep_size is not None else basis.number_spins
    for i, current in enumerate(
        islice(
            sampler,
            options.number_discarded * sweep_size,
            (options.number_discarded + options.number_samples) * sweep_size,
            sweep_size,
        )
    ):
        states[i] = current.state
        log_prob[i] = current.log_prob
    return states, log_prob, sampler.acceptance_rate


def metropolis_process(
    initial_state: Tensor,
    log_prob_fn: Callable[[Tensor], Tensor],
    kernel_fn: Callable[[Tensor], Tuple[Tensor, Tensor]],
    number_samples: int,
    number_discarded: int,
    sweep_size: int,
) -> Tuple[Tensor, Tensor, Tensor]:
    assert number_samples >= 1
    current_state, current_norm = initial_state
    current_log_prob = log_prob_fn(current_state)
    if current_log_prob.dim() > 1:
        current_log_prob.squeeze_(dim=1)
    states = current_state.new_empty((number_samples,) + current_state.size())
    log_probs = current_log_prob.new_empty((number_samples,) + current_log_prob.size())
    accepted = torch.zeros(current_state.size(0), dtype=torch.int64, device=current_state.device)

    states[0].copy_(current_state)
    log_probs[0].copy_(current_log_prob)
    current_state = states[0]
    current_log_prob = log_probs[0]

    def sweep():
        nonlocal accepted
        for i in range(sweep_size):
            proposed_state, proposed_norm = kernel_fn(current_state)
            proposed_log_prob = log_prob_fn(proposed_state)
            if proposed_log_prob.dim() > 1:
                proposed_log_prob.squeeze_(dim=1)
            r = torch.rand(current_state.size(0)) * proposed_norm / current_norm
            t = (proposed_norm > 0) & (r <= torch.exp(proposed_log_prob - current_log_prob))
            current_state[t] = proposed_state[t]
            current_log_prob[t] = proposed_log_prob[t]
            current_norm[t] = proposed_norm[t]
            accepted += t

    # Thermalisation
    for i in range(number_discarded):
        sweep()

    # Reset acceptance count after thermalisation
    accepted.fill_(0)
    for i in range(1, number_samples):
        states[i].copy_(current_state)
        log_probs[i].copy_(current_log_prob)
        current_state = states[i]
        current_log_prob = log_probs[i]
        sweep()

    # Subtract 1 because the loop above starts at 1
    acceptance = accepted.double() / ((number_samples - 1) * sweep_size)
    print("Information :: Acceptance {}".format(torch.mean(acceptance)))
    return states, log_probs, acceptance


def sample_using_metropolis(log_prob_fn, basis, options):
    initial_state = _prepare_initial_state(basis, options.number_chains)
    initial_norm = basis.norm(initial_state).to(options.device)
    initial_state = initial_state.to(options.device)
    kernel_fn = MetropolisKernel(basis)
    states, log_probs, acceptance = metropolis_process(
        (initial_state, initial_norm),
        log_prob_fn,
        kernel_fn,
        options.number_samples,
        options.number_discarded,
        options.sweep_size,
    )
    info = {"acceptance_rate": torch.mean(acceptance).item()}
    return states, log_probs, info


def _sample_exactly(log_ψ: Callable[[Tensor], Tensor], basis, options: SamplingOptions):
    r"""Samples states from the Hilbert space basis ``basis`` according to the
    probability distribution proportional to ‖ψ‖².

    We compute ``‖ψ(s)‖²`` for all states ``s`` in ``basis`` and then directly
    sample from this discrete probability distribution.

    Number of samples is ``options.number_chains * options.number_samples``,
    and ``options.number_discarded`` is ignored, since there is no need for
    thermalisation.
    """
    device = options.device

    def make_xs_and_ys():
        basis.build()  # Initialises internal cache if not done already
        # Compute log amplitudes
        xs = torch.from_numpy(basis.states.view(np.int64)).to(device)
        # ys are log amplitudes on all states xs
        ys = forward_with_batches(log_ψ, xs, batch_size=8192).squeeze()
        if ys.dim() != 1:
            raise ValueError(
                "log_ψ should return real part of the logarithm of the "
                "wavefunction, but output tensor has dimension {}; did "
                "you by accident use sign instead of amplitude network?"
                "".format(ys.dim())
            )
        if ys.device.type != device.type:
            raise ValueError(
                "log_ψ should return tensors residing on {}; received "
                "tensors residing on {} instead".format(device, ys.device)
            )
        return xs, ys

    xs, ys = make_xs_and_ys()
    probabilities = _log_amplitudes_to_probabilities(ys)
    if len(probabilities) < (1 << 24):
        # PyTorch only supports discrete probability distributions
        # shorter than 2²⁴.
        indices = torch.multinomial(
            probabilities,
            num_samples=options.number_chains * options.number_samples,
            # replacement=True is IMPORTANT because it more closely
            # emulates the actual Monte Carlo behaviour
            replacement=True,
        )
        log_prob = probabilities[indices]
        log_prob = torch.log_(log_prob)
        states = xs[indices].to(device)
    else:
        # If we have more than 2²⁴ different probabilities chances are,
        # NumPy will complain about probabilities not being normalised
        # since float32 precision is not enough. The simplest
        # workaround is to convert the probabilities to float64 and
        # then renormalise then which is what we do.
        probabilities = probabilities.to(device="cpu", dtype=torch.float64)
        probabilities /= torch.sum(probabilities)
        cpu_indices = np.random.choice(
            len(probabilities),
            size=options.number_chains * options.number_samples,
            replace=True,
            p=probabilities,
        )
        log_prob = probabilities[cpu_indices].to(device=device, dtype=torch.float32)
        log_prob = torch.log_(log_prob)
        states = xs[cpu_indices.to(device)]
    states = torch.cat(
        [
            states.unsqueeze(dim=1),
            torch.zeros(states.size(0), 7, device=device, dtype=torch.int64),
        ],
        dim=1,
    )
    shape = (options.number_samples, options.number_chains)
    return states.view(*shape, 8), log_prob.view(*shape), None


def sample_some(
    log_ψ: Callable[[Tensor], Tensor], basis, options: SamplingOptions, mode="exact"
) -> Tuple[Tensor, Tensor, Optional[Tensor]]:
    with torch.no_grad(), torch.jit.optimized_execution(True):
        if mode == "exact":
            return _sample_exactly(log_ψ, basis, options)
        elif mode == "metropolis":
            return sample_using_metropolis(lambda x: 2 * log_ψ(x), basis, options)
        else:
            supported = {"exact", "metropolis", "zanella"}
            raise ValueError("invalid mode: {!r}; must be one of {}".format(mode, supported))


def closeness_testing_l1(p, q, n, C, m):
    from math import sqrt

    def sample(fn, n, number_samples):
        counts = np.zeros(n)
        for _ in range(number_samples):
            counts[fn() - 1] += 1
        return counts

    def experiment(number_samples):
        Z = sum(
            ((x - y) ** 2 - x - y) / (x + y)
            for x, y in zip(sample(p, n, number_samples), sample(q, n, number_samples))
            if x + y > 0
        )
        print(Z)
        return Z <= C * sqrt(number_samples)

    results = [experiment(M) for M in np.random.poisson(m, size=10)]
    return np.array(results)


def benchmark():
    from itertools import islice
    from nqs_playground._C_nqs.v2 import SpinBasis, MetropolisKernel

    basis = SpinBasis([], number_spins=8, hamming_weight=4)
    basis.build()
    prob = torch.rand(basis.number_states)
    prob /= torch.sum(prob)

    def log_prob_fn(state):
        return 0.5 * torch.log(torch.tensor([prob[basis.index(x)] for x in state]))

    kernel = MetropolisKernel(basis)
    sampler = Sampler(kernel, log_prob_fn, 1)
    for _ in islice(sampler, 1000, 100000, 2 * basis.number_spins):
        pass


def test_uniform():
    from itertools import islice
    from nqs_playground import SpinBasis

    basis = SpinBasis([], number_spins=10, hamming_weight=5)
    basis.build()

    class Target:
        def __init__(self):
            self.target_prob = torch.rand(basis.number_states)
            self.target_prob /= torch.sum(self.target_prob)
            self.buffer = None
            self._i = 0

        def log_prob(self, state):
            p = torch.tensor([self.target_prob[basis.index(x)] for x in state], dtype=torch.float32)
            return torch.log(p)

        def __call__(self):
            if self._i == 0:
                self._fill()
            self._i -= 1
            return self.buffer[self._i]

        def _fill(self):
            # +1 Here is to ensure that we count from 1
            self.buffer = (
                torch.multinomial(self.target_prob, num_samples=10240, replacement=True) + 1
            )
            self._i = len(self.buffer)

    q = Target()
    sampler = islice(
        Sampler(MetropolisKernel(basis), lambda x: q.log_prob(x), batch_size=1),
        500 * basis.number_spins,
        1000000000,
        basis.number_spins,
    )

    def p():
        state, _, _ = next(sampler)
        return basis.index(state.item()) + 1

    return closeness_testing_l1(p, q, basis.number_states, 1.0, 10000)


def test():
    from nqs_playground._C_nqs.v2 import SpinBasis

    basis = SpinBasis([], 10, 5)
    m = torch.nn.Sequential(
        torch.nn.Linear(10, 64),
        torch.nn.ReLU(),
        torch.nn.Linear(64, 64),
        torch.nn.ReLU(),
        torch.nn.Linear(64, 64),
        torch.nn.ReLU(),
        torch.nn.Linear(64, 1),
    )
    options = SamplingOptions(number_samples=100, number_chains=4)
    return _sample_using_metropolis(m, basis, options)


# @torch.jit.script
# def _step(
#     current: Tuple[Tensor, Tensor, Tensor, Tensor], proposed: Tuple[Tensor, Tensor, Tensor]
# ) -> Tuple[Tensor, Tensor, Tensor]:
#     r"""Internal function of the Sampler."""
#     state, norm, log_prob, count = current
#     proposed_state, proposed_norm, proposed_log_prob = proposed
#     if proposed_log_prob.dim() == 2:
#         proposed_log_prob = proposed_log_prob.squeeze(dim=1)
#     r = torch.rand(state.size(0)) * proposed_norm / norm
#     t = (proposed_norm > 0) & (r <= torch.exp(proposed_log_prob - log_prob))
#     state[t] = proposed_state[t]
#     norm[t] = proposed_norm[t]
#     log_prob[t] = proposed_log_prob[t]
#     count += t
#     return state, norm, log_prob, count


def _random_spin_configuration(n: int, hamming_weight: Optional[int] = None) -> int:
    if hamming_weight is not None:
        assert 0 <= hamming_weight and hamming_weight <= n, "invalid hamming weight"
        bits = ["1"] * hamming_weight + ["0"] * (n - hamming_weight)
        np.random.shuffle(bits)
        return int("".join(bits), base=2)
    else:
        return randint(0, 1 << n - 1)


def _prepare_initial_state(basis, batch_size: int) -> torch.Tensor:
    r"""Generates a batch of valid spin configurations (i.e. representatives).
    """
    if batch_size <= 0:
        raise ValueError("invalid batch size: {}; expected a positive integer".format(batch_size))
    # First, we generate a bunch of representatives, and then sample uniformly
    # from them.
    states = set()
    for _ in range(max(2 * batch_size, 10000)):
        spin = _random_spin_configuration(basis.number_spins, basis.hamming_weight)
        states.add(basis.full_info(spin)[0])

    def to_array(x, out):
        for i in range(8):
            out[i] = x & 0xFFFFFFFFFFFFFFFF
            x >>= 64

    states = list(states)
    out = torch.empty((batch_size, 8), dtype=torch.int64)
    batch = out.numpy().view(np.uint64)
    for i, index in enumerate(torch.randperm(len(states))[:batch_size]):
        to_array(states[index], batch[i])
    return out


@torch.jit.script
def _log_amplitudes_to_probabilities(values: Tensor) -> Tensor:
    prob = values - torch.max(values)
    prob *= 2
    prob = torch.exp_(prob)
    prob /= torch.sum(prob)
    return prob

def autocorr_function(x: np.ndarray) -> np.ndarray:
    r"""Estimate the normalised autocorrelation function of a 1D array.

    :param x:
    :return: 
    """
    if x.ndim != 1:
        raise ValueError("x has wrong shape: {}; expected a 1D array".format(x.shape))
    n = 1 << math.ceil(math.log2(len(x)))
    f = np.fft.fft(x - np.mean(x), n=2 * n)
    autocorr = np.fft.ifft(f * np.conj(f))[: len(x)].real
    autocorr /= autocorr[0]
    return autocorr


def auto_window(taus, c):
    m = np.arange(len(taus)) < c * taus
    if np.any(m):
        return np.argmin(m)
    return len(taus) - 1


def integrated_autocorr_time(x: np.ndarray, c: float = 5.0) -> np.ndarray:
    if isinstance(x, torch.Tensor):
        x = x.numpy()
    f = np.zeros(x.shape[0])
    for i in range(x.shape[1]):
        f += autocorr_function(x[:, i])
    f /= x.shape[1]
    taus = 2.0 * np.cumsum(f) - 1.0
    window = auto_window(taus, c)
    return taus[window]
