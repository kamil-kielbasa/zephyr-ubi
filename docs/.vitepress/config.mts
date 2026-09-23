import { defineConfig } from 'vitepress'

const slug = process.env.GITHUB_REPOSITORY || 'kamil-kielbasa/zephyr-ubi'
const name = slug.split('/')[1]
const repoUrl = `https://github.com/${slug}`
const apiUrl = `${repoUrl}/blob/main/include/ubi/ubi.h`

export default defineConfig({
  title: 'zephyr-ubi',
  description: 'A volume manager for raw NOR flash, built as a Zephyr module',
  base: `/${name}/`,
  cleanUrls: true,

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
          { text: 'Examples', link: '/examples' }
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
