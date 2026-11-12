import { RouterProvider, createRouter } from "@tanstack/react-router";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { routerBasepath } from "./lib/base";
import { routeTree } from "./routeTree.gen";
import "./styles.css";

// The app can live under Coder's path prefix, so the router's base path is
// read from the document, never hard-coded.
const router = createRouter({ routeTree, basepath: routerBasepath() });

declare module "@tanstack/react-router" {
  interface Register {
    router: typeof router;
  }
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <RouterProvider router={router} />
  </StrictMode>,
);
