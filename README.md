
I wanted to have a heartbeat visualised via an LED using a microwave sensor. 







I noticed that when testing the sensor's physical boundaries, the heartbeat display behaves drastically differently depending on the direction of movement. When moving straight backwards, the light shuts off exactly as expected at the threshold. However, when moving sideways, the display gets stuck blinking indefinitely. On inspection, the code seems to behave like this: instead of actively clearing the target data when tracking is lost, it permanently holds onto the last recorded value. When a distance packet arrives (for example, at 80 cm), the program stores it in the latestDistance variable. If the user moves sideways well beyond 130 cm and exits the sensor's tracking zone, the radar simply stops sending new distance data. Because no new data overwrites it, the program continues to remember latestDistance = 80 cm indefinitely.
When that happens, then the software falls into a logical trap. It evaluates the stale data, concludes that a person is still sitting inside the 130 cm zone, and keeps the heartbeat display running. The problem isn’t the 130 cm cutoff logic itself—the code is doing exactly what it was written to do based on the data it has. The root cause is a software "blind spot" where the system fails to realize it has lost the target entirely, mistaking a lack of new updates for a stationary user.
So, I updated the code to allow for this by introducing a distance freshness timer to track the age of the incoming data. Rather than treating a stale value as a live presence, the system now requires continuous validation. When a distance packet arrives, the code accepts it and resets a tracking timer. If the sensor stops providing fresh data for a brief grace period—about 1.5 to 2 seconds—the software formally declares the target as lost, clears the stored distance, and turns off the heartbeat display. This creates a necessary third state in our logic: we now explicitly distinguish between a person being inside the zone, a person being too far away, and tracking being completely lost. This fixes the sideways boundary issue without altering the hardware configuration, the 130 cm threshold, or the existing pulse patterns.
