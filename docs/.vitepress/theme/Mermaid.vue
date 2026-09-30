<script lang="ts">
let diagrams = 0
</script>

<script setup lang="ts">
import { onMounted, ref, watch } from 'vue'
import { useData } from 'vitepress'

// Drawn in the browser, from the source the markdown carries.
const props = defineProps<{ code: string }>()
const { isDark } = useData()
const id = `mermaid-${++diagrams}`
const svg = ref('')

async function draw() {
  const { default: mermaid } = await import('mermaid')

  mermaid.initialize({
    startOnLoad: false,
    securityLevel: 'strict',
    theme: isDark.value ? 'dark' : 'default'
  })

  const drawn = await mermaid.render(id, decodeURIComponent(props.code))

  svg.value = drawn.svg
}

onMounted(draw)
watch(isDark, draw)
</script>

<template>
  <div class="mermaid" v-html="svg" />
</template>

<style scoped>
.mermaid {
  display: flex;
  justify-content: center;
  margin: 16px 0;
}
</style>
