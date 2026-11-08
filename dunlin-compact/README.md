# Dunlin Compact

The lightweight C11 Agent runtime within **Dunlin C**. `kernel/` implements scope, service and event primitives; `runtime/` composes Agent services; `plugins/` provides model protocols and Agent loop; `platform/` implements OS facilities. Public headers live under `../include/dunlin/compact/`. Desktop-only modules stay outside this component. The public API uses the `dunlin_*` namespace.
