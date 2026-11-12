import { createFileRoute } from "@tanstack/react-router";

// "/" is the Sim GUI itself. The canvas lives in the root layout so that it
// survives route changes; this route adds nothing on top of it.
export const Route = createFileRoute("/")({ component: () => null });
