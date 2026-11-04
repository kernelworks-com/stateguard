# Security

Stateguard is a diagnostic tool. It checks recorded lifecycle events after the
fact; it is not a security boundary and does not prevent a device or backend
from writing to memory. A `clean` result covers only the supplied events and
modeled rules.

## Reporting a problem

Open a [GitHub issue](https://github.com/kernelworks-com/stateguard/issues) with
a small synthetic trace that reproduces it, the command you ran, and the result
you expected. `stateguard-reduce` and `stateguard-generate` can help produce
one.

Never attach production captures. Traces carry identifiers and structural
metadata; remove private paths, credentials, and customer data from logs too.

This is an experimental project maintained on a best-effort basis. There is no
private disclosure or embargo process, and no response time is promised.
