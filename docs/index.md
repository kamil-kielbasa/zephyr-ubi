---
layout: home

hero:
  name: zephyr-ubi
  text: UBI for Zephyr
  tagline: A volume manager for raw NOR flash, with wear levelling, power-loss safety and authenticated metadata.
  actions:
    - theme: brand
      text: How it works
      link: /how-it-works
    - theme: alt
      text: Examples
      link: /examples

features:
  - title: What it does
    details: Volumes on raw NOR flash, atomic block updates, appends, and wear levelling that runs when the application asks for it.
    link: /how-it-works
    linkText: How it works
  - title: Security
    details: Headers and the volume table are authenticated with AES-CMAC. What is caught, by whom, and where the boundary lies.
    link: /security
    linkText: Threat model
  - title: Examples
    details: Adding the module, a first volume, appending records and keeping maintenance up.
    link: /examples
    linkText: Code
---
