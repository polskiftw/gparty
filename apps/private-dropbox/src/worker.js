import { PAGE_HTML } from "./page.js";
import APP_JS from "./app.js";
import APP_CSS from "./style.css";

export function authenticated(request) {
  const cert = request.cf?.tlsClientAuth;
  return cert?.certPresented === "1" && cert?.certVerified === "SUCCESS" && cert?.certRevoked === "0";
}
