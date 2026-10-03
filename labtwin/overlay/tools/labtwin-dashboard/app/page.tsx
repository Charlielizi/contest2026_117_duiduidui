import type { Metadata } from "next";
import { BoardAdminApp } from "../board/BoardAdminApp";

export const metadata: Metadata = {
  title: "LabTwin 实验看板",
  description: "本地优先、证据可追溯的实验状态与环境事件看板。",
};

export default function Home() {
  return <BoardAdminApp />;
}
