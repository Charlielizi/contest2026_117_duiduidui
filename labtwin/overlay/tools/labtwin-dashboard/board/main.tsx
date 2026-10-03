import React from "react";
import ReactDOM from "react-dom/client";
import { BoardAdminApp } from "./BoardAdminApp";
import "./board.css";
import "../app/globals.css";
import "./unified.css";

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <BoardAdminApp />
  </React.StrictMode>,
);
