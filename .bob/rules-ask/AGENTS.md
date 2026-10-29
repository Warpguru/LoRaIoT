# Project Documentation Context (Non-Obvious Only)

- The entire system architecture, hardware wiring, register maps, and frame protocol are documented in `doc/Feasibility.md` - this is the **canonical reference**, not just a proposal.
- `pom.xml` and `.classpath` are Eclipse Maven project scaffolding artifacts. They do not represent the actual build system - there are no Maven plugins or Java source files.
- The `.settings/org.eclipse.jdt.core.prefs` Java 1.8 target applies only to the Eclipse IDE shell, not to the firmware.
- `doc/E220-400T22D_UserManual_EN_v1.2.pdf` is the hardware datasheet for the LoRa module - consult it for register-level details beyond what `doc/Feasibility.md` summarizes.
- The project uses **LoRa P2P (not LoRaWAN)** - the E220 module is a transparent UART bridge, not a LoRaWAN node. This distinction is critical when answering protocol questions.
