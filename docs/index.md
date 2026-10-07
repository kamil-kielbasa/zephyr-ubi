---
layout: home

hero:
  name: UBI for Zephyr
  text: Unsorted Block Images
  tagline: Named volumes on raw flash, with wear levelling, power-cut safety and authenticated metadata.
  actions:
    - theme: brand
      text: How it works
      link: /how-it-works
    - theme: alt
      text: Examples
      link: /examples

features:
  - title: Volumes on raw flash
    details: Named volumes of logical blocks, created, resized and removed at run time. A block is replaced atomically, or appended to without an erase.
    link: /how-it-works#logical-and-physical-blocks
    linkText: Logical and physical blocks
  - title: Wear levelling
    details: Erases are spread over the whole partition, on a budget the application chooses.
    link: /how-it-works#wear-levelling
    linkText: Wear levelling
  - title: Power-cut safety
    details: Block updates, volume changes, erases and maintenance leave the old state or the new one.
    link: /operations#power-loss
    linkText: Power loss
  - title: Authenticated metadata
    details: Headers and the volume table carry an AES-CMAC. Tampering is reported apart from damage, and the state callback lets the application detect a rollback.
    link: /security
    linkText: Security
  - title: Examples
    details: Adding the module, attaching, volumes, appends, key provisioning and maintenance on a work queue.
    link: /examples
    linkText: Examples
  - title: Operations
    details: What each error and event means, what a power cut leaves behind, when to run maintenance, resources and limits.
    link: /operations
    linkText: Operations
---
