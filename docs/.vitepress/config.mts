import { defineConfig } from 'vitepress'

const slug = process.env.GITHUB_REPOSITORY || 'kamil-kielbasa/zephyr-ubi'
const name = slug.split('/')[1]
const repoUrl = `https://github.com/${slug}`
const apiUrl = `${repoUrl}/blob/main/include/ubi/ubi.h`

export default defineConfig({
  title: 'UBI for Zephyr',
  description: 'A volume manager for raw flash, built as a Zephyr module',
  base: `/${name}/`,
  cleanUrls: true,

  // A mermaid fence becomes a diagram, drawn in the browser.
  markdown: {
    config(md) {
      const fence = md.renderer.rules.fence!

      md.renderer.rules.fence = (tokens, idx, options, env, self) => {
        const token = tokens[idx]

        if (token.info.trim() === 'mermaid')
          return `<Mermaid code="${encodeURIComponent(token.content)}" />`

        return fence(tokens, idx, options, env, self)
      }
    }
  },

  // Mermaid's own chunks are that large; they load only with a diagram.
  vite: {
    build: { chunkSizeWarningLimit: 1024 }
  },

  themeConfig: {
    nav: [
      { text: 'Guide', link: '/how-it-works' },
      { text: 'Examples', link: '/examples' },
      { text: 'API', link: apiUrl }
    ],

    sidebar: [
      {
        text: 'Guide',
        items: [
          { text: 'How it works', link: '/how-it-works' },
          { text: 'Security', link: '/security' },
          { text: 'Examples', link: '/examples' },
          { text: 'Operations', link: '/operations' }
        ]
      },
      {
        text: 'Reference',
        items: [
          { text: 'On-flash format', link: '/on-flash-format' },
          { text: 'API', link: apiUrl }
        ]
      }
    ],

    socialLinks: [{ icon: 'github', link: repoUrl }],

    search: { provider: 'local' },

    footer: {
      message: 'Released under the MIT License.'
    }
  }
})
