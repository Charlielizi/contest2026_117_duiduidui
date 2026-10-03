import type { Metadata } from "next";
import "../board/board.css";
import "./globals.css";
import "../board/unified.css";

export const metadata: Metadata = {
  title: "LabTwin 实验看板",
  description: "Gemini-S1 实验状态、环境事件与可追溯报告看板。",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN" suppressHydrationWarning>
      <head>
        <script
          dangerouslySetInnerHTML={{
            __html: `
              (function () {
                try {
                  const saved = localStorage.getItem('labtwin-theme');
                  const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
                  const theme = saved === 'dark' || saved === 'light' ? saved : prefersDark ? 'dark' : 'light';
                  document.documentElement.setAttribute('data-theme', theme);
                } catch (e) {}
              })();
            `,
          }}
        />
      </head>
      <body>{children}</body>
    </html>
  );
}
