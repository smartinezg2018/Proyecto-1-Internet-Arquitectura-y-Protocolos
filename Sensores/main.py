from Sensores.iot_simulator import Sensores

sens = Sensores()

print(f"{'Temp':<8} | {'Energía':<10} | {'Vibración':<10} | {'Humedad':<8} | {'Estado'}")
print("-" * 65)

for _ in range(20):
    t = sens.temperature()
    e = sens.energy_consumption()
    v = sens.mechanical_vibration()
    h = sens.humidity()
    s = sens.operational_status()
    
    print(f"{t:>5}°C  | {e:>7} kWh | {v:>7} mm/s | {h:>6}%  | {s}")