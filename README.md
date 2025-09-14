Robot Player Unit
Bluetooth ps3 controller, Websocket, Servo, Switch, Led and motor dc

update skull basher robot pemukul

We need the log_death.php file to report every death event or hit input from an object (robot) to the system. This is the basis for generating information for the scoreboard.
Some functions in log_death are:
1. HMAC Security
2. Receiving hit reports from each of your robots
3. Clearing the database table after a death has passed (respawn)
4. Filtering robot and turret deaths
5. Building a JSON file that can be read by get_score
6. Filtering input data from special robots, such as (hitting robots with additional critical damage) through attack_buffer.json

This log_attack is a special feature of the basher robot. This file contains a special data request for the skull basher skill, which allows the basher robot to receive additional damage and a 2-second stun effect. This data must be injected into the JSON file. It can then be read by log_death for each hit event.

This robot_status function indicates whether the robot and turret are active or inactive. If the robot is hit and freezes for a certain number of seconds or its health is nearly depleted, it will send a death signal.
