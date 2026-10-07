import type { Theme } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import Mermaid from './Mermaid.vue'
import Zoom from './Zoom.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () =>
    h(DefaultTheme.Layout, null, { 'layout-bottom': () => h(Zoom) }),
  enhanceApp({ app }) {
    app.component('Mermaid', Mermaid)
  }
} satisfies Theme
