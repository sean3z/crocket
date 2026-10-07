// Rocket 0.5 serving the scenarios of bench/server.cpp, for ./dev bench --vs-rocket.
// Same routes, same bodies; Rocket's defaults (its Shield on, logging off via env).
#[macro_use]
extern crate rocket;
use rocket::serde::{json::Json, Deserialize, Serialize};
use std::time::Duration;

#[derive(Serialize)]
#[serde(crate = "rocket::serde")]
struct Message { message: &'static str }

#[derive(Serialize, Deserialize)]
#[serde(crate = "rocket::serde")]
struct Item { sku: String, name: String, qty: i32, price: f64 }

#[derive(Serialize, Deserialize)]
#[serde(crate = "rocket::serde")]
struct Order { id: u64, email: String, #[serde(default)] tags: Vec<String>, #[serde(default)] items: Vec<Item> }

#[get("/plaintext")]
fn plaintext() -> &'static str { "Hello, World!" }

#[get("/json")]
fn json() -> Json<Message> { Json(Message { message: "Hello, World!" }) }

#[post("/orders", data = "<order>")]
fn orders(order: Json<Order>) -> Json<Order> { order }

// A blocking 20 ms wait, the way Rocket recommends blocking work: on tokio's blocking pool.
#[get("/wait")]
async fn wait() -> &'static str {
    rocket::tokio::task::spawn_blocking(|| std::thread::sleep(Duration::from_millis(20))).await.unwrap();
    "done"
}

#[get("/wait_async")]
async fn wait_async() -> &'static str {
    rocket::tokio::time::sleep(Duration::from_millis(20)).await;
    "done"
}

// Mounted at /api/v0 ... /api/v39, as in crocket_bench.
#[get("/users")]
fn list() -> &'static str { "[]" }
#[get("/users/<id>")]
fn get(id: u64) -> String { id.to_string() }
#[put("/users/<id>")]
fn put(id: u64) -> String { id.to_string() }
#[get("/users/<id>/orders")]
fn user_orders(id: u64) -> String { id.to_string() }
#[get("/users/<id>/orders/<order>")]
fn order(id: u64, order: u64) -> String { (id + order).to_string() }

#[launch]
fn rocket() -> _ {
    let mut r = rocket::build().mount("/", routes![plaintext, json, orders, wait, wait_async]);
    for i in 0..40 {
        r = r.mount(format!("/api/v{i}"), routes![list, get, put, user_orders, order]);
    }
    r
}
