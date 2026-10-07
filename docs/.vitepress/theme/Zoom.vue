<script setup lang="ts">
import { onBeforeUnmount, onMounted } from 'vue'
import { zoomed } from './zoom'

function close() {
  zoomed.value = ''
}

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape') close()
}

// Diagrams open themselves; images in a page open here.
function onClick(event: MouseEvent) {
  const target = event.target

  if (!(target instanceof HTMLImageElement)) return
  if (!target.closest('.vp-doc')) return

  zoomed.value = target.outerHTML
}

onMounted(() => {
  document.addEventListener('keydown', onKey)
  document.addEventListener('click', onClick)
})

onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKey)
  document.removeEventListener('click', onClick)
})
</script>

<template>
  <Teleport to="body">
    <div v-if="zoomed" class="zoom" @click="close" v-html="zoomed" />
  </Teleport>
</template>

<style>
.vp-doc img {
  cursor: zoom-in;
}

.zoom {
  position: fixed;
  inset: 0;
  z-index: 200;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  background: var(--vp-c-bg);
  cursor: zoom-out;
}

.zoom > svg,
.zoom > img {
  width: 100%;
  height: 100%;
  max-width: none !important;
  object-fit: contain;
}
</style>
