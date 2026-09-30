---
layout: home

hero:
  name: UBI for Zephyr
  text: Volumes on raw flash
  tagline: Wear levelling, power-loss safety and authenticated metadata, as a Zephyr module.
  actions:
    - theme: brand
      text: How it works
      link: /how-it-works
    - theme: alt
      text: Examples
      link: /examples

features:
  - title: What it does
    details: Named volumes, atomic block updates, appends, and wear levelling that runs when the application asks for it.
    link: /how-it-works
    linkText: How it works
  - title: Security
    details: Headers and the volume table are authenticated with AES-CMAC. What is caught, by whom, and where the boundary lies.
    link: /security
    linkText: Threat model
  - title: Examples
    details: Adding the module, attaching, volumes, appends, key provisioning and maintenance.
    link: /examples
    linkText: Code
---
