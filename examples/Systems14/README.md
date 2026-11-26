# Systems14 examples

From the Bolt source root:

```sh
bolt run examples/Systems14/Core --require-native
bolt run examples/Systems14/Threading --require-native
bolt run examples/Systems14/Network --release --require-native
```

For Process, first compile Child to a chosen path and pass its absolute path to Process:

```sh
bolt compile examples/Systems14/Child -o /tmp/BoltExampleChild --require-native
bolt run examples/Systems14/Process --require-native -- /tmp/BoltExampleChild
```

On Windows use a writable absolute `.exe` output and pass that path, keeping arguments
separate. No shell is used by Process.Start. Network is local UDP loopback and uses an
OS-assigned port; it does not contact a public host. The larger tests under tests/systems14
also exercise TCP, IPv6, faults/timeouts, output limits and cleanup.
